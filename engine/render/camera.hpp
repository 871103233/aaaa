#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace vx {

/// 角度 → 弧度换算因子（编译期常量，避免为一个常量引入运行时依赖）。
inline constexpr float kDegToRad = 0.01745329251994329577F;

/// 第三人称相机的俯仰上下限：**±89°**（弧度制）。
/// 留 1° 余量，使视线方向永不与世界上方向平行，从而避免万向节奇异。
inline constexpr float kCameraPitchLimit = 89.0F * kDegToRad;

/// 相机与注视点之间的**最小距离**（格）。
///
/// 不变量：`ThirdPersonCamera::Evaluate` 返回的 `eye` 与 `target` 的间距恒 `>=` 本值。
/// 若允许间距退化到 0（避障把相机拉到注视点、或相机被埋在实心体内后由"顶出实心"的安全网顶到注视点正上方），
/// `glm::lookAt` 的视线基向量要么是零向量、要么与世界上方向平行，归一化会得到 NaN；
/// 视图矩阵随之失效、整帧几何被丢弃 —— 画面只剩清屏色（缺陷 B2）。
inline constexpr float kCameraMinDistance = 0.5F;

/// 地形查询接口：**相机避障所需的最小契约**。
///
/// 为什么要放在引擎层：世界层（`world/`）尚未落地，而相机必须能在没有地形实现的情况下
/// 被单元测试覆盖；因此这里只声明"问什么"，**不引入任何地形专有类型**
/// （`TerrainTile`、`DigVolume` 等一律不出现）。世界层后续实现本接口
/// （分层混合世界，见 ADR 0004）。
///
/// 坐标约定：世界定位用整数与 `double`（红线 6）。本接口用 `float` **只承载查询参数**，
/// 不承担世界定位职责；相机相对偏移在上传 GPU 前完成。
///
/// 线程约定：只在逻辑线程（主线程）调用。
class ITerrainQuery {
public:
    virtual ~ITerrainQuery() = default;

    ITerrainQuery(const ITerrainQuery&) = delete;
    ITerrainQuery& operator=(const ITerrainQuery&) = delete;
    ITerrainQuery(ITerrainQuery&&) = delete;
    ITerrainQuery& operator=(ITerrainQuery&&) = delete;

    /// 查询水平位置 `(worldX, worldZ)` 处地表的最高点高度（世界单位，格）。
    /// 返回 true 表示该处有地表且 `outHeight` 有效；返回 false 表示查询范围外 / 无数据。
    [[nodiscard]] virtual bool QueryHeight(float worldX, float worldZ, float& outHeight) const = 0;

    /// 线段遮挡查询：判断从 `from` 到 `to` 的线段是否被地形阻断。
    /// 返回 true 表示被阻断，此时 `outSafeT` ∈ [0, 1] 为**从 `from` 起沿线段可安全前进的比例**
    /// （即线段上最后一个不在地形内部的点）；返回 false 表示整段畅通，`outSafeT` 置 1。
    /// 用途：第三人称相机避障——沿视线把相机拉近到比例 `outSafeT` 处。
    [[nodiscard]] virtual bool QueryObstruction(const glm::vec3& from, const glm::vec3& to,
                                                float& outSafeT) const = 0;

    /// 该点是否**在实心体内**（地表之下的地形，或被可挖体积填充的实体）。
    ///
    /// 用途：相机的"不得埋在实心里"安全网。实现**必须包含可挖体积**（ADR 0011 / 0012 的
    /// 「谁来画 / 谁来挡必须同源」原则）：体积挖出的洞在**地表高度场里仍显示为实心**，
    /// 若只用高度场判定，站进洞里的角色会把相机顶到旧地表之上 ⇒ 视角退化为俯视。
    [[nodiscard]] virtual bool IsSolid(const glm::vec3& point) const = 0;

protected:
    ITerrainQuery() = default;
};

/// 第三人称相机的可调参数（不含逐帧变化的模拟状态）。
struct CameraSettings {
    float followDistance     = 5.0F;         ///< 期望的跟随距离（格）
    float pivotHeight        = 1.6F;         ///< 注视点相对目标位置的高度（约等于胶囊体身高）
    float fieldOfViewDegrees = 70.0F;        ///< 垂直视场角（度）
    float nearPlane          = 0.05F;        ///< 近裁剪面（格）
    float farPlane           = 1000.0F;      ///< 远裁剪面（格）
    float aspectRatio        = 16.0F / 9.0F; ///< 视口宽高比
    float collisionMargin    = 0.2F;         ///< 避障时沿视线预留的安全余量（格）
    float groundClearance    = 0.2F;         ///< 相机被顶出实心体后**额外**保留的间隙（格）
};

/// 相机在某一渲染帧的求值结果。
///
/// **纯输出**：不携带任何可回写逻辑状态的字段；调用方只能拿去渲染与上传 GPU。
struct CameraView {
    glm::vec3 eye { 0.0F };              ///< 相机世界位置（上传前做相机相对偏移，红线 6）
    glm::vec3 target { 0.0F };           ///< 实际注视点（跟随目标 + 高度偏移）
    glm::mat4 view { 1.0F };             ///< 视图矩阵
    glm::mat4 projection { 1.0F };       ///< 投影矩阵（右手系，深度 0~1）
    glm::mat4 viewProjection { 1.0F };   ///< 投影 × 视图（写进相机常量缓冲）
    float     distance = 0.0F;           ///< 实际使用的跟随距离（避障后可能小于请求值）
};

/// 第三人称相机：yaw / pitch 环绕、按距离跟随目标、**沿视线避障**。
///
/// 状态划分（红线 11 —— 插值只用于渲染，不得回写逻辑状态）：
///   - **模拟状态**：`m_targetPrevious` / `m_targetCurrent` / yaw / pitch / 跟随距离。
///     只在固定逻辑步内由 `Advance` / `AddYaw` / `SetPitch` 等写入。
///   - **渲染状态**：`Evaluate(alpha, terrain)` 的返回值。它是 `const` 方法，
///     只把 `alpha` 用于在"上一逻辑步"与"当前逻辑步"之间做插值，**绝不改动任何成员**。
///
/// 线程约定：只在逻辑线程（主线程）使用。
class ThirdPersonCamera {
public:
    explicit ThirdPersonCamera(CameraSettings settings = CameraSettings {}) noexcept;

    ThirdPersonCamera(const ThirdPersonCamera&) = delete;
    ThirdPersonCamera& operator=(const ThirdPersonCamera&) = delete;

    // ---- 模拟侧写入（每个固定逻辑步调用）----

    /// 推进一个逻辑步：把当前目标变为"上一目标"，并接受新的目标位置。
    void Advance(const glm::vec3& targetPosition) noexcept;

    /// 吸附到目标（传送 / 初始化）：上一目标与当前目标同时置为 `targetPosition`，
    /// 从而本步插值不产生拖影。
    void SnapTo(const glm::vec3& targetPosition) noexcept;

    void SetYaw(float yawRadians) noexcept;
    void AddYaw(float deltaRadians) noexcept;

    /// 设置俯仰角；超限值被**钳制到 ±89°**（见 `kCameraPitchLimit`）。
    void SetPitch(float pitchRadians) noexcept;

    /// 增量调整俯仰角；结果同样钳制到 ±89°。
    void AddPitch(float deltaRadians) noexcept;

    void SetFollowDistance(float distance) noexcept;

    void SetAspectRatio(float aspectRatio) noexcept;

    // ---- 只读查询 ----

    [[nodiscard]] float Yaw() const noexcept { return m_yaw; }
    [[nodiscard]] float Pitch() const noexcept { return m_pitch; }
    [[nodiscard]] float FollowDistance() const noexcept { return m_settings.followDistance; }
    [[nodiscard]] glm::vec3 TargetCurrent() const noexcept { return m_targetCurrent; }
    [[nodiscard]] glm::vec3 TargetPrevious() const noexcept { return m_targetPrevious; }
    [[nodiscard]] const CameraSettings& Settings() const noexcept { return m_settings; }

    // ---- 渲染侧求值 ----

    /// 用渲染插值系数 `alpha`（由固定步长累加器产出）求值出本帧的相机。
    ///
    /// - `alpha` 被钳制到 `[0, 1]`（非值按 0 处理），因此**不会外插**。
    /// - `terrain` 可为空：为空时跳过避障（地形未就绪或纯几何测试）。
    /// - 避障顺序：先沿视线做线段遮挡查询并把相机**拉近**（预留 `collisionMargin` 余量），
    ///   再用 `ITerrainQuery::IsSolid` 做**安全网**（相机绝不停留在实心体内，顶出后额外留 `groundClearance`）。
    /// - 本方法 `const`：**绝不回写模拟状态**（红线 11）。
    [[nodiscard]] CameraView Evaluate(double alpha, const ITerrainQuery* terrain = nullptr) const noexcept;

private:
    /// 单位视线方向（由 yaw / pitch 决定）。
    [[nodiscard]] glm::vec3 Forward() const noexcept;

    /// `alpha` 处的注视点（在上一 / 当前目标之间插值，并抬高 `pivotHeight`）。
    [[nodiscard]] glm::vec3 PivotAt(float alpha) const noexcept;

    CameraSettings m_settings;
    glm::vec3      m_targetCurrent { 0.0F };
    glm::vec3      m_targetPrevious { 0.0F };
    float          m_yaw   = 0.0F;
    float          m_pitch = 0.0F;
};

}  // namespace vx
