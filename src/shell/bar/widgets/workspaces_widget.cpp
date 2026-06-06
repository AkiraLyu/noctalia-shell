#include "shell/bar/widgets/workspaces_widget.h"

#include "core/ui_phase.h"
#include "render/animation/animation.h"
#include "render/animation/animation_manager.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "system/app_identity.h"
#include "system/desktop_entry.h"
#include "system/internal_app_metadata.h"
#include "ui/builders.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <functional>
#include <linux/input-event-codes.h>
#include <memory>
#include <utility>
#include <wayland-client-protocol.h>

namespace {
  [[nodiscard]] bool isEmptyWorkspace(const Workspace& workspace) {
    return !workspace.occupied && !workspace.active && !workspace.urgent;
  }

  void filterEmptyWorkspaces(std::vector<Workspace>& workspaces) {
    workspaces.erase(std::remove_if(workspaces.begin(), workspaces.end(), isEmptyWorkspace), workspaces.end());
  }

  constexpr float kWorkspaceGap = Style::spaceXs;
  constexpr float kWorkspacePillDefaultHeight = Style::barGlyphSize;
  constexpr float kWorkspaceAnimDurationMs = static_cast<float>(Style::animNormal);
  constexpr std::chrono::milliseconds kApplicationsHoverLeaveDelay{150};
  constexpr float kGroupedIconGap = 2.0f;
  constexpr float kGroupedGridGap = Style::spaceSm;

  [[nodiscard]] FontWeight workspaceFontWeight(FontWeight baseWeight, bool minimal, bool active) {
    if (minimal && active) {
      return static_cast<FontWeight>(static_cast<int>(baseWeight) + 200);
    }
    return baseWeight;
  }

  [[nodiscard]] float centeredOffset(float extent, float content) {
    return std::round(std::max(0.0f, extent - content) * 0.5f);
  }

  [[nodiscard]] float oddPx(float value) {
    int px = std::max(1, static_cast<int>(std::lround(value)));
    if ((px % 2) == 0) {
      ++px;
    }
    return static_cast<float>(px);
  }

  [[nodiscard]] float workspaceCapsuleCrossExtent(float slotCross, float barScale, float fallbackScale) {
    if (slotCross <= 1.0f) {
      return std::round(kWorkspacePillDefaultHeight * fallbackScale);
    }
    const float inset = std::round(std::min(Style::spaceXs * barScale, slotCross * 0.12f));
    return std::max(1.0f, slotCross - 2.0f * inset);
  }

  [[nodiscard]] std::uintptr_t syntheticWindowKey(
      const WorkspaceWindowAssignment& assignment, std::size_t index
  ) {
    const std::string seed = assignment.windowId.empty()
        ? assignment.workspaceKey + "\n" + assignment.appId + "\n" + assignment.title + "\n" + std::to_string(index)
        : assignment.windowId;
    std::uintptr_t value = static_cast<std::uintptr_t>(std::hash<std::string>{}(seed));
    if (value == 0) {
      value = static_cast<std::uintptr_t>(index + 1);
    }
    return value;
  }
} // namespace

WorkspacesWidget::WorkspacesWidget(
    CompositorPlatform& platform, wl_output* output, DisplayMode displayMode, ColorSpec focusedColor,
    ColorSpec occupiedColor, ColorSpec emptyColor, std::size_t maxLabelChars, bool labelsOnlyWhenOccupied,
    bool hideWhenEmpty, float pillScale, bool minimal, bool showApplications, bool showApplicationsHover,
    bool colorizeIcons, float unfocusedIconsOpacity, float groupedBorderOpacity, bool enableScrollWheel,
    float iconScale, bool showBadge, float barScale
)
    : m_platform(platform), m_output(output), m_displayMode(displayMode), m_maxLabelChars(maxLabelChars),
      m_labelsOnlyWhenOccupied(labelsOnlyWhenOccupied), m_hideWhenEmpty(hideWhenEmpty), m_pillScale(pillScale),
      m_barScale(std::max(0.1f, barScale)), m_minimal(minimal), m_showApplications(showApplications),
      m_showApplicationsHover(showApplicationsHover), m_colorizeIcons(colorizeIcons),
      m_unfocusedIconsOpacity(std::clamp(unfocusedIconsOpacity, 0.0f, 1.0f)),
      m_groupedBorderOpacity(std::clamp(groupedBorderOpacity, 0.0f, 1.0f)), m_enableScrollWheel(enableScrollWheel),
      m_iconScale(std::clamp(iconScale, 0.1f, 1.0f)), m_showBadge(showBadge), m_focusedColor(std::move(focusedColor)),
      m_occupiedColor(std::move(occupiedColor)), m_emptyColor(std::move(emptyColor)) {
  buildDesktopIconIndex();
}

WorkspacesWidget::DisplayMode WorkspacesWidget::effectiveDisplayMode() const noexcept {
  if (m_minimal && m_displayMode == DisplayMode::None) {
    return DisplayMode::Id;
  }
  return m_displayMode;
}

bool WorkspacesWidget::shouldShowWorkspaceLabel(const Workspace& workspace, std::string_view label) const noexcept {
  if (effectiveDisplayMode() == DisplayMode::None || label.empty()) {
    return false;
  }
  if (m_labelsOnlyWhenOccupied && !workspace.occupied && !workspace.active) {
    return false;
  }
  return true;
}

bool WorkspacesWidget::shouldShowBarCapsule() const {
  return !m_showApplications && Widget::shouldShowBarCapsule();
}

void WorkspacesWidget::create() {
  auto container = std::make_unique<InputArea>();
  container->setOnAxis([this](const InputArea::PointerData& data) {
    if (!m_enableScrollWheel) {
      return;
    }
    if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
      return;
    }
    const float delta = data.scrollDelta(1.0f);
    if (delta == 0.0f) {
      return;
    }
    // Wayland reports positive wheel deltas for "scroll down", so treat that
    // as moving to the next workspace and negative as previous.
    activateAdjacentWorkspace(delta > 0.0f ? 1 : -1);
  });
  container->setOnEnter([this](const InputArea::PointerData&) { handlePointerEnter(); });
  container->setOnLeave([this]() { handlePointerLeave(); });
  m_container = container.get();
  setRoot(std::move(container));
}

void WorkspacesWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  m_lastContainerWidth = containerWidth;
  m_lastContainerHeight = containerHeight;
  const bool wasVertical = m_isVertical;
  m_isVertical = containerHeight > containerWidth;
  if (wasVertical != m_isVertical) {
    m_rebuildPending = true;
  }
  const bool appVisible = applicationsVisible();
  if (m_lastApplicationsVisible != appVisible) {
    m_lastApplicationsVisible = appVisible;
    m_rebuildPending = true;
  }
  const std::uint64_t textMetricsGeneration = renderer.textMetricsGeneration();
  if (m_textMetricsGeneration != textMetricsGeneration) {
    m_textMetricsGeneration = textMetricsGeneration;
    m_rebuildPending = true;
  }
  if (m_rebuildPending) {
    rebuild(renderer);
    m_rebuildPending = false;
  }
}

void WorkspacesWidget::syncWidgetVisibility(bool showWidget) {
  if (Node* rootNode = root(); rootNode != nullptr) {
    rootNode->setVisible(showWidget);
    rootNode->setParticipatesInLayout(showWidget);
  }
}

void WorkspacesWidget::doUpdate(Renderer& renderer) {
  const auto desktopVersion = desktopEntriesVersion();
  if (desktopVersion != m_desktopEntriesVersion) {
    buildDesktopIconIndex();
    m_rebuildPending = true;
  }

  auto current = m_platform.workspaces(m_output);

  if (!m_cachedState.empty()
      && !current.empty()
      && !std::any_of(current.begin(), current.end(), [](const Workspace& ws) { return ws.active; })) {
    return;
  }

  if (m_hideWhenEmpty) {
    filterEmptyWorkspaces(current);
  }

  const bool showWidget = !current.empty();
  syncWidgetVisibility(showWidget);
  if (!showWidget) {
    if (!m_cachedState.empty() || !m_items.empty()) {
      m_cachedState.clear();
      m_groupedState.clear();
      m_rebuildPending = true;
      if (root() != nullptr) {
        root()->markLayoutDirty();
      }
    }
    return;
  }

  if (applicationsVisible()) {
    const auto nextGroups = buildApplicationGroups(current);
    const bool stateChanged = current.size() != m_cachedState.size()
        || !std::equal(current.begin(), current.end(), m_cachedState.begin(), [](const Workspace& a, const Workspace& b) {
             return a.id == b.id
                 && a.name == b.name
                 && a.index == b.index
                 && a.coordinates == b.coordinates
                 && a.active == b.active
                 && a.urgent == b.urgent
                 && a.occupied == b.occupied;
           });
    if (stateChanged || !groupedModelsEqual(nextGroups)) {
      m_cachedState = current;
      m_groupedState = nextGroups;
      m_rebuildPending = true;
      if (root() != nullptr) {
        root()->markLayoutDirty();
      }
    }
    (void)renderer;
    return;
  }

  if (m_cachedState.empty() && current.empty()) {
    return;
  }

  bool structuralChange = current.size() != m_cachedState.size();
  bool activeChange = false;
  if (!structuralChange) {
    for (std::size_t i = 0; i < current.size(); ++i) {
      const auto& a = current[i];
      const auto& b = m_cachedState[i];
      if (a.id != b.id || a.name != b.name || a.index != b.index || a.coordinates != b.coordinates) {
        structuralChange = true;
        break;
      }
      if (a.active != b.active || a.urgent != b.urgent) {
        activeChange = true;
        if (m_labelsOnlyWhenOccupied) {
          structuralChange = true;
        }
      }
      if (a.occupied != b.occupied) {
        activeChange = true;
        if (m_labelsOnlyWhenOccupied) {
          structuralChange = true;
        }
      }
    }
  }

  if (!structuralChange && !activeChange) {
    return;
  }

  m_cachedState.clear();
  m_cachedState.reserve(current.size());
  for (const auto& ws : current) {
    m_cachedState.push_back(
        Workspace{
            .id = ws.id,
            .name = ws.name,
            .coordinates = ws.coordinates,
            .index = ws.index,
            .active = ws.active,
            .urgent = ws.urgent,
            .occupied = ws.occupied
        }
    );
  }

  if (structuralChange) {
    m_rebuildPending = true;
    if (root() != nullptr) {
      root()->markLayoutDirty();
    }
  } else {
    retarget(renderer);
  }
}

bool WorkspacesWidget::applicationsVisible() const noexcept {
  return m_showApplications && (!m_showApplicationsHover || m_hoverActive);
}

void WorkspacesWidget::handlePointerEnter() {
  if (!m_showApplications || !m_showApplicationsHover) {
    return;
  }
  m_hoverLeaveTimer.stop();
  setHoverActive(true);
}

void WorkspacesWidget::handlePointerLeave() {
  if (!m_showApplications || !m_showApplicationsHover) {
    return;
  }
  m_hoverLeaveTimer.start(kApplicationsHoverLeaveDelay, [this]() { setHoverActive(false); });
}

void WorkspacesWidget::setHoverActive(bool active) {
  if (m_hoverActive == active) {
    return;
  }
  m_hoverActive = active;
  m_rebuildPending = true;
  if (root() != nullptr) {
    root()->markLayoutDirty();
  }
  requestUpdate();
}

std::vector<WorkspacesWidget::WorkspaceGroup>
WorkspacesWidget::buildApplicationGroups(const std::vector<Workspace>& workspaces) {
  std::vector<WorkspaceGroup> groups;
  groups.reserve(workspaces.size());

  const auto displayKeys = m_platform.workspaceDisplayKeys(m_output);
  std::unordered_map<std::string, std::size_t> groupIndexByKey;
  auto addKey = [&](std::string key, std::size_t index) {
    if (!key.empty()) {
      groupIndexByKey.emplace(std::move(key), index);
    }
  };

  for (std::size_t i = 0; i < workspaces.size(); ++i) {
    const std::string baseKey =
        i < displayKeys.size() && !displayKeys[i].empty() ? displayKeys[i] : workspaceKeyFor(workspaces[i], i);
    WorkspaceGroup group{};
    group.workspace = workspaces[i];
    group.key = baseKey;
    group.label = groupedWorkspaceLabel(workspaces[i], i);
    groups.push_back(std::move(group));
    const std::size_t index = groups.size() - 1;
    addKey(baseKey, index);
    addKey(workspaces[i].id, index);
    addKey(workspaces[i].name, index);
  }

  const auto active = m_platform.activeToplevel();
  const auto* activeHandle = active.has_value() ? active->handle : nullptr;
  const auto focusedWindowId = m_platform.focusedCompositorWindowId();

  auto appendAssignment = [&](const WorkspaceWindowAssignment& assignment, std::size_t order) {
    if (assignment.workspaceKey.empty()) {
      return;
    }
    const auto groupIt = groupIndexByKey.find(assignment.workspaceKey);
    if (groupIt == groupIndexByKey.end()) {
      return;
    }

    WindowItem item{};
    item.handleKey = syntheticWindowKey(assignment, order);
    item.order = static_cast<std::uint64_t>(order);
    item.workspaceOrder = static_cast<std::uint64_t>(order);
    item.appId = assignment.appId;
    item.appIdLower = toLower(item.appId);
    item.startupWmClassLower = item.appIdLower;
    item.nameLower = item.appIdLower;
    item.title = assignment.title;
    item.workspaceKey = assignment.workspaceKey;
    item.workspaceWindowId = assignment.windowId;
    item.active = (focusedWindowId.has_value() && !assignment.windowId.empty() && assignment.windowId == *focusedWindowId)
        || (active.has_value()
            && !assignment.appId.empty()
            && toLower(active->appId) == item.appIdLower
            && (assignment.title.empty() || active->title == assignment.title));

    if (!item.appIdLower.empty()) {
      const auto windows = m_platform.windowsForApp(item.appIdLower, item.startupWmClassLower, m_output);
      auto match = std::find_if(windows.begin(), windows.end(), [&](const ToplevelInfo& window) {
        return !assignment.title.empty() && window.title == assignment.title;
      });
      if (match == windows.end() && windows.size() == 1) {
        match = windows.begin();
      }
      if (match != windows.end()) {
        item.firstHandle = match->handle;
        item.order = match->order;
        if (match->handle != nullptr) {
          item.handleKey = reinterpret_cast<std::uintptr_t>(match->handle);
        } else if (match->extHandle != nullptr) {
          item.handleKey = reinterpret_cast<std::uintptr_t>(match->extHandle);
        }
        if (activeHandle != nullptr && activeHandle == match->handle) {
          item.active = true;
        }
      }
    }

    item.iconPath = resolveIconPath(item.appId);
    groups[groupIt->second].windows.push_back(std::move(item));
  };

  auto assignments = m_platform.workspaceWindowAssignments(m_output);
  std::stable_sort(assignments.begin(), assignments.end(), [](const auto& lhs, const auto& rhs) {
    if (lhs.workspaceKey != rhs.workspaceKey) {
      return lhs.workspaceKey < rhs.workspaceKey;
    }
    if (lhs.x != rhs.x) {
      return lhs.x < rhs.x;
    }
    if (lhs.y != rhs.y) {
      return lhs.y < rhs.y;
    }
    return lhs.windowId < rhs.windowId;
  });

  for (std::size_t i = 0; i < assignments.size(); ++i) {
    appendAssignment(assignments[i], i);
  }

  if (assignments.empty()) {
    const auto appIdsByWorkspace = m_platform.appIdsByWorkspace(m_output);
    std::unordered_map<std::string, std::size_t> appOccurrence;
    std::size_t syntheticOrder = 0;
    for (auto& group : groups) {
      const auto byKey = appIdsByWorkspace.find(group.key);
      const auto byName = appIdsByWorkspace.find(group.workspace.name);
      const auto byId = appIdsByWorkspace.find(group.workspace.id);
      const auto* appList = byKey != appIdsByWorkspace.end()
          ? &byKey->second
          : (byName != appIdsByWorkspace.end() ? &byName->second
                                               : (byId != appIdsByWorkspace.end() ? &byId->second : nullptr));
      if (appList == nullptr) {
        continue;
      }
      for (const auto& appId : *appList) {
        const std::string appLower = toLower(appId);
        const auto windows = m_platform.windowsForApp(appLower, appLower, m_output);
        const std::size_t occurrence = appOccurrence[group.key + '\n' + appLower]++;

        WindowItem item{};
        item.handleKey = static_cast<std::uintptr_t>(std::hash<std::string>{}(
            group.key + "\n" + appId + "\n" + std::to_string(occurrence)
        ));
        if (item.handleKey == 0) {
          item.handleKey = syntheticOrder + 1;
        }
        item.order = syntheticOrder++;
        item.workspaceOrder = item.order;
        item.appId = appId;
        item.appIdLower = appLower;
        item.startupWmClassLower = appLower;
        item.nameLower = appLower;
        item.workspaceKey = group.key;
        if (occurrence < windows.size()) {
          const auto& window = windows[occurrence];
          item.title = window.title;
          item.firstHandle = window.handle;
          item.order = window.order;
          if (window.handle != nullptr) {
            item.handleKey = reinterpret_cast<std::uintptr_t>(window.handle);
          } else if (window.extHandle != nullptr) {
            item.handleKey = reinterpret_cast<std::uintptr_t>(window.extHandle);
          }
          item.active = activeHandle != nullptr && activeHandle == window.handle;
        } else if (active.has_value() && toLower(active->appId) == appLower) {
          item.active = true;
        }
        item.iconPath = resolveIconPath(item.appId);
        group.windows.push_back(std::move(item));
      }
    }
  }

  for (auto& group : groups) {
    std::stable_sort(group.windows.begin(), group.windows.end(), [](const WindowItem& lhs, const WindowItem& rhs) {
      if (lhs.workspaceOrder != rhs.workspaceOrder) {
        return lhs.workspaceOrder < rhs.workspaceOrder;
      }
      if (lhs.order != rhs.order) {
        return lhs.order < rhs.order;
      }
      return lhs.handleKey < rhs.handleKey;
    });
  }

  return groups;
}

bool WorkspacesWidget::groupedModelsEqual(const std::vector<WorkspaceGroup>& groups) const {
  if (groups.size() != m_groupedState.size()) {
    return false;
  }
  for (std::size_t i = 0; i < groups.size(); ++i) {
    const auto& a = groups[i];
    const auto& b = m_groupedState[i];
    if (a.key != b.key || a.label != b.label) {
      return false;
    }
    if (a.workspace.id != b.workspace.id
        || a.workspace.name != b.workspace.name
        || a.workspace.index != b.workspace.index
        || a.workspace.coordinates != b.workspace.coordinates
        || a.workspace.active != b.workspace.active
        || a.workspace.urgent != b.workspace.urgent
        || a.workspace.occupied != b.workspace.occupied) {
      return false;
    }
    if (a.windows.size() != b.windows.size()) {
      return false;
    }
    for (std::size_t j = 0; j < a.windows.size(); ++j) {
      const auto& aw = a.windows[j];
      const auto& bw = b.windows[j];
      if (aw.handleKey != bw.handleKey
          || aw.order != bw.order
          || aw.workspaceOrder != bw.workspaceOrder
          || aw.appId != bw.appId
          || aw.title != bw.title
          || aw.iconPath != bw.iconPath
          || aw.workspaceKey != bw.workspaceKey
          || aw.workspaceWindowId != bw.workspaceWindowId
          || aw.active != bw.active
          || aw.firstHandle != bw.firstHandle) {
        return false;
      }
    }
  }
  return true;
}

void WorkspacesWidget::rebuild(Renderer& renderer) {
  if (applicationsVisible()) {
    rebuildGroupedApplications(renderer);
    return;
  }
  rebuildPills(renderer);
}

void WorkspacesWidget::rebuildPills(Renderer& renderer) {
  uiAssertNotRendering("WorkspacesWidget::rebuild");
  cancelAnimation();
  while (!m_container->children().empty()) {
    m_container->removeChild(m_container->children().back().get());
  }
  m_items.clear();

  const auto& workspaces = m_cachedState;
  const float gap = kWorkspaceGap * m_contentScale;
  const float labelFontSize = Style::fontSizeMini * m_contentScale;
  const float slotCross = m_isVertical ? m_lastContainerWidth : m_lastContainerHeight;
  const float pillHeight =
      std::max(1.0f, std::round(workspaceCapsuleCrossExtent(slotCross, m_barScale, m_contentScale) * m_pillScale));
  const FontWeight configuredFontWeight = labelFontWeight();

  std::vector<std::string> labels;
  labels.reserve(workspaces.size());
  for (std::size_t i = 0; i < workspaces.size(); ++i) {
    labels.push_back(workspaceLabel(workspaces[i], i));
  }

  // Measure text and compute per-slot widths (v4-style: proportional to char count).
  // Width = max(baseSize * factor, textWidth + padding)
  //   factor: 2.2 for active, 1.0 for inactive
  //   padding: baseSize * 0.6
  struct SlotMetrics {
    std::string label;
    bool showLabel = false;
    bool isNumeric = false;
    float textWidth = 0.0f;
    float inkCenterOffset = 0.0f;
    float inkVCenterOffset = 0.0f;
    float inactiveWidth = 0.0f;
    float activeWidth = 0.0f;
  };
  std::vector<SlotMetrics> slots(workspaces.size());

  for (std::size_t i = 0; i < workspaces.size(); ++i) {
    auto& slot = slots[i];
    slot.label = labels[i];
    slot.showLabel = shouldShowWorkspaceLabel(workspaces[i], labels[i]);

    // Detect numeric labels (workspace IDs like "1", "10", "11")
    slot.isNumeric = !labels[i].empty() && std::all_of(labels[i].begin(), labels[i].end(), [](char c) {
      return std::isdigit(static_cast<unsigned char>(c));
    });

    if (slot.showLabel) {
      const FontWeight slotFontWeight = workspaceFontWeight(configuredFontWeight, m_minimal, workspaces[i].active);
      const TextMetrics tm = renderer.measureText(labels[i], labelFontSize, slotFontWeight);
      slot.textWidth = std::max(tm.right - tm.left, tm.inkRight - tm.inkLeft);
      const float logicalCenter = (tm.left + tm.right) * 0.5f;
      const float inkCenter = (tm.inkLeft + tm.inkRight) * 0.5f;
      slot.inkCenterOffset = slot.isNumeric ? 0.0f : (inkCenter - logicalCenter);
      const float logicalVCenter = (tm.top + tm.bottom) * 0.5f;
      const float inkVCenter = (tm.inkTop + tm.inkBottom) * 0.5f;
      slot.inkVCenterOffset = inkVCenter - logicalVCenter;
    }
  }

  const float baseSize = std::round(pillHeight);
  const float padding = m_minimal ? (Style::spaceXs * m_contentScale) : (baseSize * 0.6f);
  constexpr float kActiveFactor = 2.2f;
  constexpr float kInactiveFactor = 1.0f;

  float maxLabelHeight = labelFontSize;
  for (std::size_t i = 0; i < workspaces.size(); ++i) {
    auto& slot = slots[i];
    if (m_minimal) {
      const float minWidth = baseSize;
      if (!slot.showLabel) {
        slot.inactiveWidth = minWidth;
        slot.activeWidth = minWidth;
      } else {
        const float textBasedWidth = slot.textWidth + padding * 2.0f;
        slot.inactiveWidth = std::max(minWidth, textBasedWidth);
        slot.activeWidth = slot.inactiveWidth;
      }
      if (slot.showLabel) {
        const FontWeight slotFontWeight = workspaceFontWeight(configuredFontWeight, m_minimal, workspaces[i].active);
        const TextMetrics tm = renderer.measureText(slot.label, labelFontSize, slotFontWeight);
        maxLabelHeight = std::max(maxLabelHeight, tm.bottom - tm.top);
      }
      continue;
    }

    const float minWidth = baseSize * kInactiveFactor;
    const float minActiveWidth = baseSize * kActiveFactor;

    if (!slot.showLabel) {
      slot.inactiveWidth = minWidth;
      slot.activeWidth = minActiveWidth;
    } else {
      const float textBasedWidth = slot.textWidth + padding;
      slot.inactiveWidth = std::max(minWidth, textBasedWidth);
      slot.activeWidth = std::max(minActiveWidth, textBasedWidth);
    }
  }

  m_gap = gap;
  m_indicatorHeight = m_minimal ? std::round(maxLabelHeight + padding) : pillHeight;

  for (std::size_t i = 0; i < workspaces.size(); ++i) {
    const auto& ws = workspaces[i];
    const auto& slot = slots[i];

    auto area = std::make_unique<InputArea>();
    const float w = ws.active ? slot.activeWidth : slot.inactiveWidth;
    area->setFrameSize(w, m_indicatorHeight);

    Item item{};
    item.active = ws.active;
    item.label = slot.label;
    item.showLabel = slot.showLabel;
    item.inactiveWidth = slot.inactiveWidth;
    item.activeWidth = slot.activeWidth;
    item.inkCenterOffset = slot.inkCenterOffset;
    item.inkVCenterOffset = slot.inkVCenterOffset;

    if (!m_minimal) {
      const float indicatorW = m_isVertical ? m_indicatorHeight : w;
      const float indicatorH = m_isVertical ? w : m_indicatorHeight;
      item.indicator = static_cast<Box*>(area->addChild(
          ui::box({
              .fill = workspaceFillColor(ws),
              .radius = workspacePillRadius(indicatorW, indicatorH),
              .width = w,
              .height = m_indicatorHeight,
              .configure = [](Box& box) { box.clearBorder(); },
          })
      ));
    }

    if (slot.showLabel) {
      item.text = static_cast<Label*>(area->addChild(
          ui::label({
              .text = slot.label,
              .fontSize = labelFontSize,
              .color = workspaceTextColor(ws),
              .fontWeight = workspaceFontWeight(configuredFontWeight, m_minimal, ws.active),
              .baselineMode = m_isVertical ? std::optional<LabelBaselineMode>{LabelBaselineMode::InkCentered}
                                           : std::optional<LabelBaselineMode>{},
          })
      ));
      item.text->measure(renderer);
    }

    auto wsCopy = ws;
    area->setOnEnter([this](const InputArea::PointerData&) { handlePointerEnter(); });
    area->setOnLeave([this]() { handlePointerLeave(); });
    area->setOnClick([this, wsCopy](const InputArea::PointerData& data) {
      if (data.button == BTN_LEFT) {
        m_platform.activateWorkspace(m_output, wsCopy);
      }
    });
    item.area = static_cast<InputArea*>(m_container->addChild(std::move(area)));
    m_items.push_back(item);
  }

  // Size the container now that per-item widths are known.
  float total = 0.0f;
  for (std::size_t i = 0; i < m_items.size(); ++i) {
    const float itemWidth = (m_cachedState[i].active) ? m_items[i].activeWidth : m_items[i].inactiveWidth;
    total += itemWidth;
  }
  if (m_items.size() > 1) {
    total += gap * static_cast<float>(m_items.size() - 1);
  }
  if (m_isVertical) {
    m_container->setFrameSize(m_indicatorHeight, total);
  } else {
    m_container->setFrameSize(total, m_indicatorHeight);
  }

  // Snap to targets immediately (no animation on structural rebuild).
  computeTargets();
  for (std::size_t i = 0; i < m_items.size(); ++i) {
    auto& it = m_items[i];
    it.currentX = it.targetX;
    it.currentWidth = it.targetWidth;
    applyItemLayout(i);
  }
}

void WorkspacesWidget::rebuildGroupedApplications(Renderer& renderer) {
  uiAssertNotRendering("WorkspacesWidget::rebuildGroupedApplications");
  cancelAnimation();
  while (m_container != nullptr && !m_container->children().empty()) {
    m_container->removeChild(m_container->children().back().get());
  }
  m_items.clear();

  if (m_groupedState.empty() && !m_cachedState.empty()) {
    m_groupedState = buildApplicationGroups(m_cachedState);
  }

  if (m_container == nullptr || m_groupedState.empty()) {
    if (m_container != nullptr) {
      m_container->setFrameSize(0.0f, 0.0f);
    }
    return;
  }

  const float scale = m_contentScale;
  const float slotCross = m_isVertical ? m_lastContainerWidth : m_lastContainerHeight;
  const float crossExtent =
      std::max(1.0f, std::round(workspaceCapsuleCrossExtent(slotCross, m_barScale, scale) * m_pillScale));
  const float baseItemSize = oddPx(crossExtent * 0.8f);
  const float iconSize = oddPx(baseItemSize * m_iconScale);
  const float iconGap = std::max(1.0f, std::round(kGroupedIconGap * scale));
  const float gridGap = std::round(kGroupedGridGap * scale);
  const bool hasLabel = effectiveDisplayMode() != DisplayMode::None;
  const float horizontalPad = std::round(Style::spaceSm * scale);
  const float leadingOffset = !m_isVertical && hasLabel ? horizontalPad : 0.0f;
  const float topOffset = m_isVertical && hasLabel ? std::round(horizontalPad * 0.4f) : 0.0f;
  const ColorSpec groupFill =
      barCapsuleSpec().enabled ? barCapsuleSpec().fill : colorSpecFromRole(ColorRole::SurfaceVariant);

  float cursor = 0.0f;
  float crossMax = 0.0f;

  for (std::size_t groupIndex = 0; groupIndex < m_groupedState.size(); ++groupIndex) {
    const auto& groupModel = m_groupedState[groupIndex];
    const auto& windows = groupModel.windows;
    const bool hasWindows = !windows.empty();
    const float windowCount = std::max(1.0f, static_cast<float>(windows.size()));
    const float runMain = iconSize * windowCount + iconGap * std::max(0.0f, windowCount - 1.0f);
    const float groupWidth = m_isVertical ? crossExtent : oddPx(runMain + Style::spaceLg * scale);
    const float groupHeight = m_isVertical ? oddPx(runMain + Style::spaceLg * scale)
                                           : crossExtent;
    const float groupRadius = resolvedBarCapsuleRadius(groupWidth, groupHeight);
    const float outlineStroke = std::max(1.0f, std::round(Style::borderWidth * scale));

    auto groupArea = std::make_unique<InputArea>();
    groupArea->setFrameSize(groupWidth, groupHeight);
    groupArea->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
    const float groupX = m_isVertical ? 0.0f : std::round(leadingOffset + cursor);
    const float groupY = m_isVertical ? std::round(topOffset + cursor) : 0.0f;
    groupArea->setPosition(groupX, groupY);
    auto wsCopy = groupModel.workspace;
    groupArea->setOnClick([this, wsCopy](const InputArea::PointerData& data) {
      if (data.button == BTN_LEFT) {
        m_platform.activateWorkspace(m_output, wsCopy);
      }
    });

    auto groupBg = ui::box({
        .fill = groupFill,
        .radius = groupRadius,
        .width = groupWidth,
        .height = groupHeight,
    });
    groupArea->addChild(std::move(groupBg));

    auto groupOutline = ui::box({
        .fill = clearColorSpec(),
        .radius = groupRadius,
        .width = groupWidth,
        .height = groupHeight,
        .visible = false,
    });
    Box* groupOutlinePtr = static_cast<Box*>(groupArea->addChild(std::move(groupOutline)));
    const auto applyGroupBorder = [this, groupOutlinePtr, outlineStroke, active = groupModel.workspace.active](bool hovered) {
      if (groupOutlinePtr == nullptr) {
        return;
      }
      if (!active && !hovered) {
        groupOutlinePtr->clearBorder();
        groupOutlinePtr->setVisible(false);
        return;
      }
      const ColorSpec border = active ? colorSpecFromRole(ColorRole::Primary, m_groupedBorderOpacity)
                                      : colorSpecFromRole(ColorRole::Hover, m_groupedBorderOpacity);
      groupOutlinePtr->setBorder(border, outlineStroke);
      groupOutlinePtr->setVisible(true);
    };
    applyGroupBorder(false);

    groupArea->setOnEnter([this, applyGroupBorder](const InputArea::PointerData&) {
      handlePointerEnter();
      applyGroupBorder(true);
    });
    groupArea->setOnLeave([this, applyGroupBorder]() {
      applyGroupBorder(false);
      handlePointerLeave();
    });

    const float runStartX = m_isVertical ? centeredOffset(groupWidth, iconSize) : centeredOffset(groupWidth, runMain);
    const float runStartY = m_isVertical ? centeredOffset(groupHeight, runMain) : centeredOffset(groupHeight, iconSize);

    for (std::size_t windowIndex = 0; windowIndex < windows.size(); ++windowIndex) {
      const auto& window = windows[windowIndex];
      auto iconArea = std::make_unique<InputArea>();
      iconArea->setFrameSize(iconSize, iconSize);
      iconArea->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
      const float iconX =
          m_isVertical ? runStartX : std::round(runStartX + static_cast<float>(windowIndex) * (iconSize + iconGap));
      const float iconY =
          m_isVertical ? std::round(runStartY + static_cast<float>(windowIndex) * (iconSize + iconGap)) : runStartY;
      iconArea->setPosition(iconX, iconY);
      const std::string tooltip = !window.title.empty() ? window.title
                                                        : (!window.appId.empty() ? window.appId : std::string{});
      if (!tooltip.empty()) {
        iconArea->setTooltip(tooltip);
      }
      iconArea->setOnClick([this, window, wsCopy](const InputArea::PointerData& data) {
        if (data.button == BTN_LEFT) {
          activateWindowItem(window, wsCopy);
        }
      });

      if (!window.iconPath.empty()) {
        auto image = ui::image({
            .fit = ImageFit::Contain,
            .width = iconSize,
            .height = iconSize,
        });
        image->setOpacity(window.active ? 1.0f : m_unfocusedIconsOpacity);
        if (m_colorizeIcons && !window.active) {
          image->setAppIconColorization(
              colorSpecFromRole(isResolvedLightTheme() ? ColorRole::SurfaceVariant : ColorRole::OnSurface)
          );
        }
        image->setSourceFile(renderer, window.iconPath, static_cast<int>(std::round(48.0f * scale)), true);
        if (image->hasImage()) {
          iconArea->addChild(std::move(image));
        }
      }

      if (iconArea->children().empty()) {
        auto glyph = ui::glyph({
            .glyph = "apps",
            .glyphSize = iconSize,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        });
        glyph->setOpacity(window.active ? 1.0f : m_unfocusedIconsOpacity);
        glyph->measure(renderer);
        glyph->setPosition(centeredOffset(iconSize, glyph->width()), centeredOffset(iconSize, glyph->height()));
        iconArea->addChild(std::move(glyph));
      }

      auto indicator = ui::box({
          .fill = window.active ? colorSpecFromRole(ColorRole::Primary) : colorSpecFromRole(ColorRole::Hover),
          .radius = std::min(Style::scaledRadiusSm(scale), iconSize * 0.125f),
          .width = oddPx(iconSize * 0.25f),
          .height = std::max(2.0f, std::round(4.0f * scale)),
          .visible = window.active,
      });
      Box* indicatorPtr = static_cast<Box*>(iconArea->addChild(std::move(indicator)));
      if (indicatorPtr != nullptr) {
        indicatorPtr->setPosition(
            centeredOffset(iconSize, indicatorPtr->width()), std::round(iconSize - indicatorPtr->height() * 0.5f)
        );
      }

      iconArea->setOnEnter([this, indicatorPtr, active = window.active, applyGroupBorder](const InputArea::PointerData&) {
        handlePointerEnter();
        applyGroupBorder(true);
        if (indicatorPtr != nullptr && !active) {
          indicatorPtr->setFill(colorSpecFromRole(ColorRole::Hover));
          indicatorPtr->setVisible(true);
        }
      });
      iconArea->setOnLeave([this, indicatorPtr, active = window.active, applyGroupBorder]() {
        applyGroupBorder(false);
        if (indicatorPtr != nullptr && !active) {
          indicatorPtr->setVisible(false);
        }
        handlePointerLeave();
      });

      groupArea->addChild(std::move(iconArea));
    }

    const bool showBadge = hasLabel
        && m_showBadge
        && (!m_labelsOnlyWhenOccupied || hasWindows || groupModel.workspace.active);
    if (showBadge) {
      const std::string label = groupModel.label.empty() ? groupedWorkspaceLabel(groupModel.workspace, groupIndex)
                                                         : groupModel.label;
      const FontWeight fontWeight = labelFontWeight();
      float badgeFontSize = std::round(Style::fontSizeMini * scale);
      const TextMetrics metrics = renderer.measureText(label, badgeFontSize, fontWeight);
      const float textWidth = std::max(metrics.right - metrics.left, metrics.inkRight - metrics.inkLeft);
      const float textHeight = std::max(metrics.bottom - metrics.top, metrics.inkBottom - metrics.inkTop);
      const float minBadge = std::round(std::max(10.0f * scale, badgeFontSize) * 2.0f);
      const float badgeWidth = std::round(std::max(minBadge, textWidth + Style::spaceXs * scale * 0.5f));
      const float badgeHeight = std::round(std::max(minBadge, textHeight + Style::spaceXs * scale));
      auto badge = ui::box({
          .fill = workspaceBadgeFillColor(groupModel.workspace),
          .radius = std::min(Style::scaledRadiusLg(scale), badgeHeight * 0.5f),
          .width = badgeWidth,
          .height = badgeHeight,
      });
      badge->setPosition(
          std::round(-Style::fontSizeCaption * 0.55f * scale),
          std::round(-Style::fontSizeCaption * 0.25f * scale)
      );
      badge->setZIndex(1);
      auto badgeText = ui::label({
          .text = label,
          .fontSize = badgeFontSize,
          .color = workspaceBadgeTextColor(groupModel.workspace),
          .fontWeight = fontWeight,
      });
      badgeText->measure(renderer);
      badgeText->setPosition(
          std::round((badgeWidth - badgeText->width()) * 0.5f),
          std::round((badgeHeight - badgeText->height()) * 0.5f)
      );
      badge->addChild(std::move(badgeText));
      groupArea->addChild(std::move(badge));
    }

    m_container->addChild(std::move(groupArea));
    cursor += (m_isVertical ? groupHeight : groupWidth) + gridGap;
    crossMax = std::max(crossMax, m_isVertical ? groupWidth : groupHeight);
  }

  if (!m_groupedState.empty()) {
    cursor -= gridGap;
  }
  if (m_isVertical) {
    m_container->setFrameSize(crossMax, std::round(topOffset + std::max(0.0f, cursor)));
  } else {
    m_container->setFrameSize(std::round(leadingOffset + std::max(0.0f, cursor)), crossMax);
  }
}

void WorkspacesWidget::computeTargets() {
  float cursor = 0.0f;
  for (std::size_t i = 0; i < m_items.size(); ++i) {
    auto& it = m_items[i];
    const float w = (m_cachedState[i].active) ? it.activeWidth : it.inactiveWidth;
    it.targetX = cursor;
    it.targetWidth = w;
    it.active = m_cachedState[i].active;
    cursor += w + m_gap;
  }
}

void WorkspacesWidget::updateContainerSize() {
  if (m_container == nullptr || m_items.empty()) {
    return;
  }
  float total = 0.0f;
  for (std::size_t i = 0; i < m_items.size(); ++i) {
    total += m_items[i].currentWidth;
  }
  if (m_items.size() > 1) {
    total += m_gap * static_cast<float>(m_items.size() - 1);
  }
  if (m_isVertical) {
    m_container->setFrameSize(m_indicatorHeight, total);
  } else {
    m_container->setFrameSize(total, m_indicatorHeight);
  }
  if (Node* shell = barCapsuleShell(); shell != nullptr) {
    shell->markLayoutDirty();
  }
}

void WorkspacesWidget::retarget(Renderer& renderer) {
  for (std::size_t i = 0; i < m_items.size(); ++i) {
    auto& it = m_items[i];
    const auto& ws = m_cachedState[i];
    const std::string label = workspaceLabel(ws, i);
    const FontWeight fontWeight = workspaceFontWeight(labelFontWeight(), m_minimal, ws.active);
    const bool labelChanged = it.label != label;
    if (labelChanged) {
      it.label = label;
      if (it.text != nullptr) {
        it.text->setText(label);
      }
    }
    if (it.text != nullptr) {
      const bool weightChanged = it.text->fontWeight() != fontWeight;
      it.text->setFontWeight(fontWeight);
      if (labelChanged || weightChanged) {
        it.text->measure(renderer);
        const float fontSize = it.text->fontSize();
        const TextMetrics tm = renderer.measureText(label, fontSize, fontWeight);
        const float logCenter = (tm.left + tm.right) * 0.5f;
        const float inkCenter = (tm.inkLeft + tm.inkRight) * 0.5f;
        it.inkCenterOffset = inkCenter - logCenter;
        const float logVCenter = (tm.top + tm.bottom) * 0.5f;
        const float inkVCenter = (tm.inkTop + tm.inkBottom) * 0.5f;
        it.inkVCenterOffset = inkVCenter - logVCenter;
      }
    }
    if (it.indicator != nullptr) {
      it.indicator->setFill(workspaceFillColor(ws));
      it.indicator->clearBorder();
    }
    if (it.text != nullptr) {
      it.text->setColor(workspaceTextColor(ws));
    }
  }

  if (m_minimal) {
    computeTargets();
    for (std::size_t i = 0; i < m_items.size(); ++i) {
      auto& it = m_items[i];
      it.currentX = it.targetX;
      it.currentWidth = it.targetWidth;
      applyItemLayout(i);
    }
    updateContainerSize();
    if (root() != nullptr) {
      root()->markPaintDirty();
    }
    return;
  }

  for (auto& it : m_items) {
    it.fromX = it.currentX;
    it.fromWidth = it.currentWidth;
  }
  computeTargets();
  startAnimation();
}

void WorkspacesWidget::startAnimation() {
  auto* mgr = m_animations;
  if (mgr == nullptr) {
    for (std::size_t i = 0; i < m_items.size(); ++i) {
      auto& it = m_items[i];
      it.currentX = it.targetX;
      it.currentWidth = it.targetWidth;
      applyItemLayout(i);
    }
    updateContainerSize();
    return;
  }
  cancelAnimation();
  m_animId = mgr->animate(
      0.0f, 1.0f, kWorkspaceAnimDurationMs, Easing::EaseOutCubic,
      [this](float t) {
        for (std::size_t i = 0; i < m_items.size(); ++i) {
          auto& it = m_items[i];
          it.currentX = it.fromX + (it.targetX - it.fromX) * t;
          it.currentWidth = it.fromWidth + (it.targetWidth - it.fromWidth) * t;
          applyItemLayout(i);
        }
        updateContainerSize();
        if (root() != nullptr) {
          root()->markPaintDirty();
        }
      },
      [this]() { m_animId = 0; }, this
  );
  if (root() != nullptr) {
    root()->markPaintDirty();
  }
}

void WorkspacesWidget::cancelAnimation() {
  if (m_animId != 0 && m_animations != nullptr) {
    m_animations->cancel(m_animId);
  }
  m_animId = 0;
}

void WorkspacesWidget::applyItemLayout(std::size_t i) {
  auto& it = m_items[i];
  if (it.area == nullptr) {
    return;
  }
  if (m_isVertical) {
    it.area->setPosition(0.0f, std::round(it.currentX));
    it.area->setFrameSize(m_indicatorHeight, it.currentWidth);
    if (it.indicator != nullptr) {
      it.indicator->setFrameSize(m_indicatorHeight, it.currentWidth);
    }
  } else {
    it.area->setPosition(std::round(it.currentX), 0.0f);
    it.area->setFrameSize(it.currentWidth, m_indicatorHeight);
    if (it.indicator != nullptr) {
      it.indicator->setFrameSize(it.currentWidth, m_indicatorHeight);
    }
  }
  if (it.text != nullptr) {
    it.text->setVisible(it.showLabel);
    if (it.showLabel) {
      const float itemW = m_isVertical ? m_indicatorHeight : it.currentWidth;
      const float itemH = m_isVertical ? it.currentWidth : m_indicatorHeight;
      const float textX = std::round((itemW - it.text->width()) * 0.5f - it.inkCenterOffset);
      const float textY = std::round((itemH - it.text->height()) * 0.5f - it.inkVCenterOffset);
      it.text->setPosition(std::max(0.0f, textX), textY);
    }
  }
  if (it.indicator != nullptr) {
    const float itemW = m_isVertical ? m_indicatorHeight : it.currentWidth;
    const float itemH = m_isVertical ? it.currentWidth : m_indicatorHeight;
    it.indicator->setFrameSize(itemW, itemH);
    it.indicator->setRadius(workspacePillRadius(itemW, itemH));
  }
}

float WorkspacesWidget::workspacePillRadius(float width, float height) const noexcept {
  return resolvedBarCapsuleRadius(width, height);
}

WorkspacesWidget::~WorkspacesWidget() { cancelAnimation(); }

std::optional<std::size_t> WorkspacesWidget::activeWorkspaceIndex() const {
  for (std::size_t i = 0; i < m_cachedState.size(); ++i) {
    if (m_cachedState[i].active) {
      return i;
    }
  }
  return std::nullopt;
}

void WorkspacesWidget::activateAdjacentWorkspace(int direction) {
  if (m_cachedState.empty() || direction == 0) {
    return;
  }

  const auto active = activeWorkspaceIndex();
  std::size_t targetIndex = 0;
  if (!active.has_value()) {
    targetIndex = direction > 0 ? 0 : (m_cachedState.size() - 1);
  } else {
    const std::size_t current = *active;
    if (direction > 0) {
      if (current + 1 >= m_cachedState.size()) {
        return;
      }
      targetIndex = current + 1;
    } else {
      if (current == 0) {
        return;
      }
      targetIndex = current - 1;
    }
  }

  m_platform.activateWorkspace(m_output, m_cachedState[targetIndex]);
}

std::string WorkspacesWidget::workspaceLabel(const Workspace& workspace, std::size_t displayIndex) const {
  const DisplayMode displayMode = effectiveDisplayMode();
  if (displayMode == DisplayMode::Id) {
    if (workspace.index > 0) {
      return std::to_string(workspace.index);
    }
    if (const auto numericId = numericWorkspaceId(workspace); numericId.has_value()) {
      return std::to_string(*numericId);
    }
    return std::to_string(displayIndex + 1);
  }
  if (displayMode == DisplayMode::Name) {
    std::string label = !workspace.name.empty() ? workspace.name : workspace.id;
    // Only truncate non-numeric labels (words like "VESKTOP" → "VE").
    // Numeric labels (workspace IDs like "10", "11") stay as-is.
    const bool isNumeric = !label.empty()
        && std::all_of(label.begin(), label.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
    if (!isNumeric && m_maxLabelChars > 0) {
      label = StringUtils::truncateUtf8CodePoints(label, m_maxLabelChars);
    }
    return label;
  }
  return {};
}

std::optional<std::size_t> WorkspacesWidget::numericWorkspaceId(const Workspace& workspace) {
  const auto parseLeadingNumber = [](const std::string& value) -> std::optional<std::size_t> {
    if (value.empty() || !std::isdigit(static_cast<unsigned char>(value.front()))) {
      return std::nullopt;
    }

    std::size_t parsed = 0;
    std::size_t index = 0;
    while (index < value.size() && std::isdigit(static_cast<unsigned char>(value[index]))) {
      parsed = (parsed * 10) + static_cast<std::size_t>(value[index] - '0');
      ++index;
    }
    return parsed > 0 ? std::optional<std::size_t>(parsed) : std::nullopt;
  };

  if (const auto id = parseLeadingNumber(workspace.id); id.has_value()) {
    return id;
  }
  if (const auto name = parseLeadingNumber(workspace.name); name.has_value()) {
    return name;
  }
  return std::nullopt;
}

std::string WorkspacesWidget::toLower(std::string value) { return StringUtils::toLower(std::move(value)); }

std::string WorkspacesWidget::workspaceKeyFor(const Workspace& workspace, std::size_t index) const {
  if (const auto numericId = numericWorkspaceId(workspace); numericId.has_value()) {
    return std::to_string(*numericId);
  }
  if (!workspace.id.empty()) {
    return workspace.id;
  }
  if (!workspace.name.empty()) {
    return workspace.name;
  }
  if (!workspace.coordinates.empty()) {
    return std::to_string(static_cast<std::size_t>(workspace.coordinates.front()) + 1U);
  }
  return std::to_string(index + 1);
}

std::string WorkspacesWidget::groupedWorkspaceLabel(const Workspace& workspace, std::size_t displayIndex) const {
  const DisplayMode displayMode = effectiveDisplayMode();
  if (displayMode == DisplayMode::Name) {
    std::string label = !workspace.name.empty() ? workspace.name : workspace.id;
    const bool isNumeric = !label.empty()
        && std::all_of(label.begin(), label.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
    if (!isNumeric && m_maxLabelChars > 0) {
      label = StringUtils::truncateUtf8CodePoints(label, m_maxLabelChars);
    }
    return label.empty() ? std::to_string(displayIndex + 1) : label;
  }
  if (workspace.index > 0) {
    return std::to_string(workspace.index);
  }
  if (const auto numericId = numericWorkspaceId(workspace); numericId.has_value()) {
    return std::to_string(*numericId);
  }
  return std::to_string(displayIndex + 1);
}

void WorkspacesWidget::buildDesktopIconIndex() {
  m_appIconsByLower.clear();
  const auto& entries = desktopEntries();
  for (const auto& entry : entries) {
    if (entry.icon.empty()) {
      continue;
    }
    if (!entry.id.empty()) {
      m_appIconsByLower[toLower(entry.id)] = entry.icon;
    }
    if (!entry.startupWmClass.empty()) {
      m_appIconsByLower[toLower(entry.startupWmClass)] = entry.icon;
    }
    if (!entry.nameLower.empty()) {
      m_appIconsByLower[entry.nameLower] = entry.icon;
    }
  }
  m_desktopEntriesVersion = desktopEntriesVersion();
}

std::string WorkspacesWidget::resolveIconPath(const std::string& appId, const std::string& iconNameOrPath) {
  const int iconTargetSize = static_cast<int>(std::round(48.0f * m_contentScale));

  auto resolveIconName = [this, iconTargetSize](const std::string& name) -> std::string {
    if (name.empty()) {
      return {};
    }
    return m_iconResolver.resolve(name, iconTargetSize);
  };

  if (!iconNameOrPath.empty()) {
    if (const std::string primary = resolveIconName(iconNameOrPath); !primary.empty()) {
      return primary;
    }
  }

  if (appId.starts_with("steam_app_")) {
    const app_identity::DesktopEntryLookupOptions steamLookup{
        .includeHidden = true,
        .includeNoDisplay = true,
    };
    if (const auto entry = app_identity::findDesktopEntry(appId, desktopEntries(), steamLookup);
        entry.has_value() && !entry->icon.empty()) {
      if (const std::string steamIcon = resolveIconName(entry->icon); !steamIcon.empty()) {
        return steamIcon;
      }
    }
  }

  if (const auto internal = internal_apps::metadataForAppId(appId); internal.has_value()) {
    return internal->iconPath;
  }

  const std::string appIdLower = toLower(appId);
  const auto it = m_appIconsByLower.find(appIdLower);
  if (it != m_appIconsByLower.end()) {
    if (const std::string desktopIcon = resolveIconName(it->second); !desktopIcon.empty()) {
      return desktopIcon;
    }
  }
  if (!appId.empty()) {
    if (const std::string appIcon = resolveIconName(appId); !appIcon.empty()) {
      return appIcon;
    }
  }
  return m_iconResolver.resolve("application-x-executable", iconTargetSize);
}

void WorkspacesWidget::activateWindowItem(const WindowItem& item, const Workspace& fallbackWorkspace) {
  if (item.firstHandle != nullptr) {
    m_platform.activateToplevel(item.firstHandle);
    return;
  }
  if (!item.workspaceWindowId.empty()) {
    m_platform.focusCompositorWindow(item.workspaceWindowId);
    return;
  }
  m_platform.activateWorkspace(m_output, fallbackWorkspace);
}

ColorSpec WorkspacesWidget::workspaceFillColor(const Workspace& workspace) const {
  if (workspace.active) {
    return m_focusedColor;
  }
  if (workspace.urgent) {
    return colorSpecFromRole(ColorRole::Error);
  }
  if (workspace.occupied) {
    return m_occupiedColor;
  }
  ColorSpec color = m_emptyColor;
  color.alpha *= 0.55f;
  return color;
}

ColorSpec WorkspacesWidget::workspaceBadgeFillColor(const Workspace& workspace) const {
  if (workspace.active) {
    return m_focusedColor;
  }
  if (workspace.urgent) {
    return colorSpecFromRole(ColorRole::Error);
  }
  if (workspace.occupied) {
    return m_occupiedColor;
  }
  return m_emptyColor;
}

ColorSpec WorkspacesWidget::workspaceTextColor(const Workspace& workspace) const {
  if (workspace.urgent) {
    return m_minimal ? colorSpecFromRole(ColorRole::Error) : colorSpecFromRole(ColorRole::OnError);
  }
  if (!m_minimal) {
    return readableColorForFill(workspaceFillColor(workspace));
  }
  if (workspace.active) {
    return m_focusedColor;
  }
  if (workspace.occupied) {
    return m_occupiedColor;
  }
  ColorSpec color = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurfaceVariant));
  color.alpha *= 0.55f;
  return color;
}

ColorSpec WorkspacesWidget::workspaceBadgeTextColor(const Workspace& workspace) const {
  if (workspace.urgent) {
    return colorSpecFromRole(ColorRole::OnError);
  }
  return readableColorForFill(workspaceBadgeFillColor(workspace));
}

ColorRole WorkspacesWidget::onRoleForFill(ColorRole fill) {
  switch (fill) {
  case ColorRole::Primary:
    return ColorRole::OnPrimary;
  case ColorRole::Secondary:
    return ColorRole::OnSecondary;
  case ColorRole::Tertiary:
    return ColorRole::OnTertiary;
  case ColorRole::Error:
    return ColorRole::OnError;
  case ColorRole::Surface:
  case ColorRole::SurfaceVariant:
  case ColorRole::Outline:
  case ColorRole::Shadow:
  case ColorRole::Hover:
  case ColorRole::OnPrimary:
  case ColorRole::OnSecondary:
  case ColorRole::OnTertiary:
  case ColorRole::OnError:
  case ColorRole::OnSurface:
  case ColorRole::OnSurfaceVariant:
  case ColorRole::OnHover:
    return ColorRole::OnSurface;
  }
  return ColorRole::OnSurface;
}

ColorSpec WorkspacesWidget::readableColorForFill(const ColorSpec& fill) {
  if (fill.role.has_value()) {
    return colorSpecFromRole(onRoleForFill(*fill.role));
  }
  return fixedColorSpec(readableTextColorForBackground(resolveColorSpec(fill)));
}
