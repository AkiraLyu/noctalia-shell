#pragma once

#include "compositors/compositor_platform.h"
#include "core/timer_manager.h"
#include "render/animation/animation_manager.h"
#include "shell/bar/widget.h"
#include "system/icon_resolver.h"
#include "ui/palette.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class Box;
class InputArea;
class Label;
struct zwlr_foreign_toplevel_handle_v1;

class WorkspacesWidget : public Widget {
public:
  enum class DisplayMode : std::uint8_t {
    None,
    Id,
    Name,
  };

  WorkspacesWidget(
      CompositorPlatform& platform, wl_output* output, DisplayMode displayMode, ColorSpec focusedColor,
      ColorSpec occupiedColor, ColorSpec emptyColor, std::size_t maxLabelChars, bool labelsOnlyWhenOccupied,
      bool hideWhenEmpty, float pillScale, bool minimal, bool showApplications, bool showApplicationsHover,
      bool colorizeIcons, float unfocusedIconsOpacity, float groupedBorderOpacity, bool enableScrollWheel,
      float iconScale, bool showBadge, float barScale
  );
  ~WorkspacesWidget() override;

  void create() override;
  [[nodiscard]] bool shouldShowBarCapsule() const override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void rebuild(Renderer& renderer);
  void rebuildPills(Renderer& renderer);
  void rebuildGroupedApplications(Renderer& renderer);
  void computeTargets();
  void retarget(Renderer& renderer);
  void updateContainerSize();
  void startAnimation();
  void cancelAnimation();
  void applyItemLayout(std::size_t i);
  [[nodiscard]] float workspacePillRadius(float width, float height) const noexcept;
  [[nodiscard]] std::optional<std::size_t> activeWorkspaceIndex() const;
  void activateAdjacentWorkspace(int direction);

  [[nodiscard]] static std::optional<std::size_t> numericWorkspaceId(const Workspace& workspace);
  [[nodiscard]] std::string workspaceLabel(const Workspace& workspace, std::size_t displayIndex) const;
  [[nodiscard]] bool shouldShowWorkspaceLabel(const Workspace& workspace, std::string_view label) const noexcept;
  [[nodiscard]] DisplayMode effectiveDisplayMode() const noexcept;
  void syncWidgetVisibility(bool showWidget);
  [[nodiscard]] bool applicationsVisible() const noexcept;
  void handlePointerEnter();
  void handlePointerLeave();
  void setHoverActive(bool active);

  struct Item {
    InputArea* area = nullptr;
    Box* indicator = nullptr;
    Label* text = nullptr;
    std::string label;
    bool showLabel = false;
    bool active = false;
    float inactiveWidth = 0.0f;
    float activeWidth = 0.0f;
    float inkCenterOffset = 0.0f;
    float inkVCenterOffset = 0.0f;
    float fromX = 0.0f;
    float fromWidth = 0.0f;
    float targetX = 0.0f;
    float targetWidth = 0.0f;
    float currentX = 0.0f;
    float currentWidth = 0.0f;
  };

  struct WindowItem {
    std::uintptr_t handleKey = 0;
    std::uint64_t order = 0;
    std::uint64_t workspaceOrder = std::numeric_limits<std::uint64_t>::max();
    std::string appId;
    std::string appIdLower;
    std::string startupWmClassLower;
    std::string nameLower;
    std::string title;
    std::string iconPath;
    std::string workspaceKey;
    std::string workspaceWindowId;
    bool active = false;
    zwlr_foreign_toplevel_handle_v1* firstHandle = nullptr;
  };

  struct WorkspaceGroup {
    Workspace workspace;
    std::string key;
    std::string label;
    std::vector<WindowItem> windows;
  };

  [[nodiscard]] std::vector<WorkspaceGroup> buildApplicationGroups(const std::vector<Workspace>& workspaces);
  [[nodiscard]] bool groupedModelsEqual(const std::vector<WorkspaceGroup>& groups) const;
  void buildDesktopIconIndex();
  [[nodiscard]] std::string resolveIconPath(const std::string& appId, const std::string& iconNameOrPath = {});
  [[nodiscard]] std::string workspaceKeyFor(const Workspace& workspace, std::size_t index) const;
  [[nodiscard]] std::string groupedWorkspaceLabel(const Workspace& workspace, std::size_t index) const;
  void activateWindowItem(const WindowItem& item, const Workspace& fallbackWorkspace);

  [[nodiscard]] ColorSpec workspaceFillColor(const Workspace& workspace) const;
  [[nodiscard]] ColorSpec workspaceBadgeFillColor(const Workspace& workspace) const;
  [[nodiscard]] ColorSpec workspaceTextColor(const Workspace& workspace) const;
  [[nodiscard]] ColorSpec workspaceBadgeTextColor(const Workspace& workspace) const;
  [[nodiscard]] static ColorRole onRoleForFill(ColorRole fill);
  [[nodiscard]] static ColorSpec readableColorForFill(const ColorSpec& fill);
  [[nodiscard]] static std::string toLower(std::string value);

  CompositorPlatform& m_platform;
  wl_output* m_output = nullptr;
  DisplayMode m_displayMode = DisplayMode::None;
  std::size_t m_maxLabelChars = 1;
  bool m_labelsOnlyWhenOccupied = false;
  bool m_hideWhenEmpty = false;
  float m_pillScale = 1.0f;
  float m_barScale = 1.0f;
  bool m_minimal = false;
  bool m_showApplications = false;
  bool m_showApplicationsHover = false;
  bool m_colorizeIcons = false;
  float m_unfocusedIconsOpacity = 1.0f;
  float m_groupedBorderOpacity = 1.0f;
  bool m_enableScrollWheel = true;
  float m_iconScale = 0.8f;
  bool m_showBadge = true;
  Node* m_container = nullptr;
  std::vector<Workspace> m_cachedState;
  std::vector<WorkspaceGroup> m_groupedState;
  std::vector<Item> m_items;
  bool m_rebuildPending = true;
  std::uint64_t m_textMetricsGeneration = 0;
  bool m_lastApplicationsVisible = false;
  bool m_hoverActive = false;

  float m_gap = 0.0f;
  float m_indicatorHeight = 0.0f;
  bool m_isVertical = false;
  float m_lastContainerWidth = 0.0f;
  float m_lastContainerHeight = 0.0f;

  AnimationManager::Id m_animId = 0;
  ColorSpec m_focusedColor = colorSpecFromRole(ColorRole::Primary);
  ColorSpec m_occupiedColor = colorSpecFromRole(ColorRole::Secondary);
  ColorSpec m_emptyColor = colorSpecFromRole(ColorRole::Secondary);
  std::unordered_map<std::string, std::string> m_appIconsByLower;
  std::uint64_t m_desktopEntriesVersion = 0;
  IconResolver m_iconResolver;
  Timer m_hoverLeaveTimer;
};
