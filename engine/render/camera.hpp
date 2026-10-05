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
/// 若允许间距退化到 0（避障把相机拉到注视点），`glm::lookAt` 的视线基向量会退化为零向量，
/// 归一化得到 NaN；视图矩阵随之失效、整帧几何被丢弃 —— 画面只剩清屏色（缺陷 B2）。
///
/// 取 **0.2** 对齐 Unity Cinemachine `CinemachineCollider::MinimumDistanceFromTarget` 的默认值：
/// 窄缝里相机必须能收到比身后岩壁更近，才能**只沿悬臂收缩而保持朝向**
/// （W6c：不得再有任何"改变朝向"的兜底，例如把相机抬到注视点正上方 ⇒ 俯视）。
inline constexpr float kCameraMinDistance = 0.2F;

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

    /// **带半径**的遮挡查询（W6f，球投射探针）：把相机近似为半径 `radius` 的球，而非一个点。
    ///
    /// 为什么需要：单**线段**查询只在"视轴"一条线上取样，薄墙可以从相机旁边"擦过"而漏检 ⇒ 相机穿墙。
    /// 业界（Unity Cinemachine `CinemachineDeoccluder::CameraRadius`、UE `USpringArmComponent` 的 probe 扫描）
    /// 都用**带体积**的探针。
    ///
    /// 语义与 `QueryObstruction` 相同（返回是否被阻断 + 最后一个安全比例），但判定把 `radius` 计入。
    /// **默认实现退回 `QueryObstruction`**（无半径）——既有实现与测试桩无需改动即可继续工作。
    [[nodiscard]] virtual bool QueryObstructionWithRadius(const glm::vec3& from, const glm::vec3& to,
                                                          float /*radius*/, float& outSafeT) const {
        return QueryObstruction(from, to, outSafeT);
    }

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

    /// **球投射探针半径**（W6f；格）：遮挡查询把相机当成半径 `cameraProbeRadius` 的球，而非一个点。
    /// 用途：薄墙不再能从相机旁"擦过"而漏检（对齐 Cinemachine `CinemachineDeoccluder::CameraRadius`）。
    /// `0` = 退回纯线段查询（旧行为）。
    float cameraProbeRadius = 0.25F;

    /// **肩位偏移**（W6g；格）：注视点沿**相机右方**（`right = (cos yaw, 0, −sin yaw)`）平移该值，
    /// 使主角偏出画面中心（over-the-shoulder）。用途：半封闭空间里避免"镜头塌到角色身上"。
    /// `0` = 居中（旧行为）。**只平移注视点，不改变朝向**（悬臂方向仍由 yaw / pitch 决定）。
    float shoulderOffset = 0.0F;

    /// **淡出主角**（W6e）的距离区间（格）：相机与注视点的距离 `d` 在
    /// `[targetFadeEndDistance, targetFadeStartDistance]` 内时，主角不透明度从 1 线性降到 0；
    /// `d >= targetFadeStartDistance` ⇒ 完全不透明（1）；`d <= targetFadeEndDistance` ⇒ 完全淡出（0）。
    /// 用途：极近距时主角不再被近裁剪面切开（"看到人物内部"），而是**平滑淡出**（片元 Bayer dither）。
    /// 判据走纯函数 `FollowTargetFadeOpacity`。
    float targetFadeStartDistance = 1.5F;
    float targetFadeEndDistance   = 0.4F;

    /// **避障推远阻尼**（W6h；秒）：遮挡消失后，跟随距离按**指数平滑**推远（帧率无关 `1 − exp(−dt/τ)`）。
    /// `0` = 立即回推（旧行为）。对齐 Cinemachine `CinemachineDeoccluder::Damping`。
    float avoidanceExtendDamping = 0.25F;

    /// **避障迟滞 —— 最短清晰时间**（W6h；秒）：遮挡消失后须**持续**这么久才允许开始推远。
    /// 用途：临界点（遮挡刚好出现/消失）反复横跳时**不回推** ⇒ 消除"距离瞬间跳变 ⇒ 画面闪烁"。
    /// `0` = 无迟滞。对齐 Cinemachine `MinimumOcclusionTime`。
    float avoidanceClearHold = 0.2F;

    /// **推远最大速度**（W6h；格/秒）：给平滑再加一道限幅，防"回拉过快"的泵感。`0` = 不限速。
    float avoidanceExtendMaxSpeed = 12.0F;

    /// 相机与注视点近到该值以下时，**跟随目标（主角）应被隐藏**（格）。
    ///
    /// **`0` = 关闭该行为（永不隐藏）**：判据 `view.distance < 0` 恒为 false
    /// （`view.distance` 恒 `>= kCameraMinDistance`）。**W6d（2026-10-06）起游戏层（`game/main.cpp`）取 `0`**
    /// —— 所有者裁定"取消镜头拉近时隐藏角色"，但**保留本能力代码**（可一键改回 1.5 重新启用）。
    ///
    /// 为什么当初需要：避障会把相机沿视线压到很近（甚至 `kCameraMinDistance`），此时相机会**落进角色体内**，
    /// 近裁剪面切开模型 ⇒ 玩家看到"人物内部"。业界通行做法（UE5 SpringArm + 隐藏 pawn、Unity Cinemachine + 角色淡出）
    /// 就是在悬臂被压得很短时"隐藏"（本仓库实现）或"淡出"主角；**关闭该行为后此现象会重新出现**（已登记为 W6d 已知后果）。
    /// 启用取值应大于"角色半径 + 近裁剪面"（默认 1.5 格，覆盖胶囊半径 0.3 与头部附近）。
    float targetHideDistance = 1.5F;
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

    /// **LOD 原点**在**渲染相对**坐标下的位置（xyz；w 预留，见下）。
    ///
    /// W7-S3b（[ADR 0024](../../docs/adr/0024-terrain-streaming-and-lod.md)）：地表 LOD 的 CDLOD 顶点过渡
    /// 按顶点到该原点的 **Chebyshev 距离**算 morph 因子（见 `assets/shaders/mesh.vert` 与 `MeshTransformUniform`）。
    /// 它是"**玩家所在 tile 的中心**"（tile 对齐 ⇒ CPU 侧 tile 距离判定与着色器距离判定同源）。
    /// 缺省 `(0,0,0)`（无 morph ⇒ 与从前逐位一致）。
    glm::vec3 lodOrigin { 0.0F };
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

    /// **推进避障平滑（W6h）**：**每个渲染帧调用一次**（在 `Evaluate` 之前）。
    ///
    /// 计算本帧的"期望跟随距离"（带半径的遮挡查询 + "不得埋在实心内"安全网），再按 `frameDt` 做
    /// **非对称阻尼 + 迟滞**：**拉近立即**（安全优先，绝不留墙内）、**推远先迟滞 `avoidanceClearHold` 秒
    /// 再指数平滑**（`avoidanceExtendDamping` / `avoidanceExtendMaxSpeed`）。
    /// 目的：临界点（遮挡刚好出现 / 消失）反复横跳时**不再让距离瞬间跳变** ⇒ 消除画面闪烁。
    ///
    /// 为什么单独一个方法、而不是在 `Evaluate` 里做：`Evaluate` 每帧被调用**多次**（渲染 + 瞄准），
    /// 必须是 `const` / 幂等的纯求值（红线 11：渲染求值绝不回写状态）；平滑是**有状态的推进**，须显式调用一次。
    /// `terrain` 为空 ⇒ 视为无遮挡（期望距离 = `followDistance`）。
    /// 调用后 `Evaluate` 使用平滑结果；**从未调用过**本方法时 `Evaluate` 退回瞬时避障（旧行为）。
    void UpdateAvoidance(float frameDt, double alpha, const ITerrainQuery* terrain) noexcept;

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
    /// - **避障距离（W6h）**：若曾调用过 `UpdateAvoidance`，用其**平滑后**的距离（不再逐帧瞬时跳变）；
    ///   否则退回**瞬时**避障 —— 先沿视线做**带半径**的遮挡查询（`QueryObstructionWithRadius`，`cameraProbeRadius`）
    ///   并把相机**拉近**（预留 `collisionMargin` 余量）。
    /// - **安全网（两条路径都生效）**：相机落在实心体内时**继续沿视线朝注视点收缩**
    ///   （绝不改变朝向，收缩极限 `kCameraMinDistance`，W6c）。
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

    /// W6h：避障平滑状态（由 `UpdateAvoidance` 每渲染帧推进；`Evaluate` 只**读**）。
    /// `m_avoidanceDistance < 0` = 尚未初始化 ⇒ `Evaluate` 退回**瞬时避障**旧路径（既有单测因此不受影响）。
    float m_avoidanceDistance   = -1.0F;
    /// W6h：迟滞计时（秒）—— 遮挡消失后累计的时间；达 `avoidanceClearHold` 才允许推远。
    float m_avoidanceClearTimer = 0.0F;
};

/// 相机过近 ⇒ **跟随目标（主角）应被隐藏**（纯函数，可单测）。
///
/// 判据：`view.distance < settings.targetHideDistance`。
/// 为什么需要：避障会把相机沿视线压到很近（甚至 `kCameraMinDistance`），此时相机会**落进角色体内**、
/// 近裁剪面切开模型 ⇒ 玩家"看到人物内部"。业界标准是在悬臂很短时隐藏 / 淡出主角；
/// **本仓库当前实现"隐藏"**（淡出需给蒙皮管线加 alpha 变体，登记为遗留）。
[[nodiscard]] bool ShouldHideFollowTarget(const CameraView& view, const CameraSettings& settings) noexcept;

/// **淡出主角**不透明度（W6e，纯函数，可单测）∈ [0, 1]。
///
/// 判据：`d = view.distance`；
///   `d >= targetFadeStartDistance` ⇒ 1（完全不透明）；
///   `d <= targetFadeEndDistance`   ⇒ 0（完全淡出）；
///   区间内线性下降（`targetFadeEndDistance >= targetFadeStartDistance` 时退化为阶跃：`d < start ⇒ 0`）。
///
/// 为什么需要：极近距时相机会落进角色体内、近裁剪面切开模型 ⇒ "看到人物内部"。业界对"贴脸穿模"的标准解是
/// **淡出主角**（dither / fade）而非硬隐藏（硬隐藏有 pop 感）。本仓库的淡出在**片元侧用 Bayer 抖动 discard**
/// 实现（见 `assets/shaders/mesh.frag`），故这里只给出不透明度这一**纯数值**，不依赖渲染后端。
[[nodiscard]] float FollowTargetFadeOpacity(const CameraView& view, const CameraSettings& settings) noexcept;

}  // namespace vx
