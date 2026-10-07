#pragma once

#include "generation/map_preset.hpp"
#include "generation/terrain_noise.hpp"
#include "render/camera.hpp"
#include "save/world_save.hpp"  // `HeightDirtyEntry`：存档差量与本类的内存表示**同源**（ADR 0037 决策四）
#include "terrain/material_table.hpp"
#include "terrain/terrain_mesher.hpp"
#include "terrain/terrain_tile.hpp"
#include "terrain/terrain_tile_source.hpp"
#include "terrain/terrain_types.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace vx {

/// 按 `(种子隐含在 noise, 生成参数隐含在 noise, 预设编辑, tile 坐标)` 生成一个完整 `TerrainTile` 的**纯自由函数**。
///
/// 顺序固定为「噪声先行、编辑覆盖其上」（与既有 `TerrainWorld::GenerateTile` 逐字相同）：
///   ① `GenerateTerrainTile`（噪声）→ ② `ApplyMapEditsToTile`（预设编辑，按文件顺序）→ ③ 刷新 `maxSurfaceBlocks` 缓存。
///
/// 抽出来的目的（W7-S3b / [ADR 0022](../../docs/adr/0022-volume-build-worker-pipeline.md) 形态）：
/// 让**worker 侧**能在**完全不触碰 `TerrainWorld` 可变状态**的前提下独立生成一个 tile
/// （worker 各自持有一份由 `(seed, params)` 构造的 `TerrainNoiseGenerator`；该生成器接口全为
/// `const noexcept`、无共享可变状态 ⇒ 可安全并发只读）。
///
/// 前置条件：`noise` 的生命周期覆盖本调用。纯函数（红线 7）：同输入 ⇒ 逐位同输出。
[[nodiscard]] TerrainTile GenerateTerrainTileData(const TerrainNoiseGenerator& noise,
                                                  const std::vector<MapEdit>& edits, int tileX, int tileZ);

/// 按 `tile.heights` **重算** `tile.maxSurfaceBlocks` 缓存（无其它副作用）。
///
/// **为什么是公开自由函数**：worker（`TerrainTileBuildPipeline`）与主线程（`TerrainWorld`）都要在
/// "tile 高度刚被填好（生成**或**读预制）之后"刷新这个缓存 —— 两条路径**共用同一份实现**才不会出现
/// "谁忘了刷新、谁的阴影投射体盒就偏小"这类分叉。
void RefreshTileMaxSurfaceBlocks(TerrainTile& tile) noexcept;

/// 地表世界：持有已加载 tile 的高度数据与网格，并实现引擎的 `ITerrainQuery` 契约。
///
/// 职责边界（ADR 0004 层 ①）：
///   - 只管理**地表高度场**；可挖体积（层 ②）与物件 / 建造（层 ③）不在此处；
///   - 生成是纯函数；单 tile 的加载 / 卸载 / 流式调度由后续流式层驱动，本类只提供单 tile 操作；
///   - **不直接调用平台 API**，依赖方向为 world → engine。
///
/// 线程约定：生成 / 笔刷 / 重网格是**单写者**写操作，在逻辑线程；生成完成后 `const` 查询与
/// `BuildTerrainMesh` 可被后台网格化线程调用。写操作不得与查询 / 网格化并发。
class TerrainWorld final : public ITerrainQuery {
public:
    /// 前置条件：`worldSeed` 即全局世界种子；`materials` 已成功加载
    /// （见 `TerrainMaterialTable::LoadFromFile`）；`params` 为地表生成参数（W3 起含地貌分区），
    /// **默认 = 引入 W3 之前的三层噪声数值**（⇒ 既有调用方与既有世界逐位不变）。
    TerrainWorld(std::uint64_t worldSeed, TerrainMaterialTable materials,
                 TerrainGenerationParams params = TerrainGenerationParams::Default());

    TerrainWorld(const TerrainWorld&) = delete;
    TerrainWorld& operator=(const TerrainWorld&) = delete;
    TerrainWorld(TerrainWorld&&) = delete;
    TerrainWorld& operator=(TerrainWorld&&) = delete;

    /// 设置预设地图的地形编辑（T11）。必须在 `GenerateTile` **之前**调用：
    /// 生成固定为「噪声先行、编辑覆盖其上」，因此每个 tile 生成后都会应用同一份编辑，
    /// 同一文件 + 同一种子 ⇒ 同一世界（红线 7）。
    void SetMapPreset(const MapPreset& preset);

    /// 设置**层间交接过滤器**（T8 / ADR 0011）：命中该过滤器的地表四边形不发射，改由可挖体积网格绘制。
    ///
    /// 必须在 `LoadTile` / `MeshTile` **之前**调用（只影响后续网格化；已网格化的 tile 需重新 `MeshTile`）。
    /// 过滤器由调用方持有，其**生命周期必须覆盖本对象**；传 `nullptr` 恢复"地表网格全覆盖"的旧行为。
    /// 之所以用接口而非直接持有可挖区域表：保持依赖方向 `dig → terrain`（地表网格化不反向依赖挖掘模块）。
    void SetQuadFilter(const ITerrainQuadFilter* filter) noexcept { m_quadFilter = filter; }

    /// 设置**地表数据来源**（V4 / [ADR 0026](../../docs/adr/0026-premade-map-format-and-bake-tool.md)）：
    /// `nullptr`（缺省）= 按种子**程序化生成** ⇒ 与引入本能力之前**逐位一致**。
    ///
    /// 前置条件：必须在任何 `GenerateTile` / `LoadTile` **之前**调用（只影响后续生成）。
    /// 生命周期：来源对象必须覆盖本对象（本类只持裸指针、**不拥有**）。
    /// 来源缺该 tile（`FillTileHeights` 返回 false）⇒ **回退程序化生成**（由来源侧负责告警，不静默失败）。
    void SetTileSource(const ITerrainTileSource* source) noexcept { m_tileSource = source; }

    // ---- 单 tile 生命周期（由流式层调用）----

    /// 生成一个 tile 的高度数据（纯函数）。该 tile 已存在时按生成结果覆盖。
    void GenerateTile(int tileX, int tileZ);

    /// 为已生成的 tile 构建网格并缓存，覆盖该 tile 之前的网格。
    /// 前置条件：该 tile 已 `GenerateTile`；未生成时为无操作。
    /// `lodLevel` 透传给 `BuildTerrainMesh`（W7-S3b）；默认 0 ⇒ 与从前逐位一致。
    void MeshTile(int tileX, int tileZ, int lodLevel = 0);

    /// `GenerateTile` + `MeshTile`。`lodLevel` 透传给 `MeshTile`（默认 0 = 全细节）。
    void LoadTile(int tileX, int tileZ, int lodLevel = 0);

    /// **卸载**一个 tile（高度数据 + 网格一并释放）。返回是否**原本常驻**（`false` = 本就不在）。
    ///
    /// 用途：流式常驻集合的"卸"（W7 / [ADR 0024](../../docs/adr/0024-terrain-streaming-and-lod.md)）。
    /// **已编辑的 tile 由调用方负责不卸**（ADR 0020 决策五口径的"脏块留驻"）——
    /// 本类不记录"是否被玩家改过"，故不做该判断。
    bool UnloadTile(int tileX, int tileZ);

    [[nodiscard]] bool HasTile(int tileX, int tileZ) const noexcept;
    [[nodiscard]] const TerrainTile* FindTile(int tileX, int tileZ) const noexcept;
    [[nodiscard]] const TerrainTileMesh* FindMesh(int tileX, int tileZ) const noexcept;

    /// 当前常驻 tile 数（**O(1)**；调试面板与"常驻量只随窗口变化"的核对用）。
    [[nodiscard]] std::size_t ResidentTileCount() const noexcept { return m_tiles.size(); }

    /// 当前常驻 tile 坐标，**升序**（流式调度算集合差用）。
    [[nodiscard]] std::vector<TileCoord> ResidentTiles() const;

    // ---- W7-S3b：worker 预取缓存 + 安装（ADR 0022/0024）----

    /// worker 产出的**已生成、未过滤**的 tile（高度 + 网格）暂存条目。
    ///
    /// 网格的层间交接过滤**不在 worker 做**（过滤器依赖**当前常驻集合**这一可变状态，
    /// 放进 worker 必须快照 ⇒ 结果随快照陈旧而有歧义），改由 `LoadTile` **安装时在主线程**按
    /// 当前 `m_quadFilter` 应用（`ApplyQuadFilterToMesh`）⇒ 与同步路径**逐位一致**。
    struct StagedTerrainTile {
        TerrainTile     tile {};
        TerrainTileMesh mesh {};
    };

    /// 把 worker 产出（`{tile, mesh}`）塞进**预取缓存**（覆盖同坐标旧条目）。
    /// 之后 `LoadTile` 命中缓存 ⇒ **只做安装**（廉价），否则回退同步生成。
    void StageTile(TerrainTile tile, TerrainTileMesh mesh);

    /// 预取缓存中是否存在 **LOD 匹配**的条目（`lodLevel` 必须一致，否则安装时会 LOD 不符）。
    [[nodiscard]] bool HasStagedTile(int tileX, int tileZ, int lodLevel) const noexcept;

    /// 预取缓存中的条目数（观测：预取提前量 / 内存有界）。
    [[nodiscard]] std::size_t StagedTileCount() const noexcept { return m_staged.size(); }

    /// 清空预取缓存（**整批作废**）。
    ///
    /// P6-B（2026-10-06）起，运行期的"窗口中心变化"**不再**走这里（整批作废会让 worker 刚算好的瓦片全白算，
    /// 飞行时表现为可见 pop-in），改用下面的 `PruneStagedTiles`。本函数保留给"确实要全清"的场合。
    void ClearStagedTiles() noexcept { m_staged.clear(); }

    /// 按谓词**裁剪**预取缓存：`keep(tileX, tileZ, lodLevel) == false` 的条目丢弃，其余保留。
    ///
    /// P6-B（2026-10-06）：窗口中心变化时**不再整批作废**（原 `ClearStagedTiles` 会把 worker 刚算好的
    /// 一整圈预取全丢掉 —— 飞行时窗口每跨一个 tile 就丢一次 ⇒ 表现为**可见的 pop-in**）。改为只丢弃
    /// "真的作废"的条目：① 已出常驻窗口（永远装不上，白占预取提前量 / 内存）；② 暂存 LOD 与目标 LOD 不符
    /// （`LoadTile` 命中不了，只能同步重建）。
    ///
    /// 谓词用模板参数（不引入 `std::function` 依赖；稳态零分配）。
    template <typename KeepFn>
    void PruneStagedTiles(KeepFn&& keep) {
        for (auto it = m_staged.begin(); it != m_staged.end();) {
            if (keep(it->first.x, it->first.z, it->second.mesh.lodLevel)) {
                ++it;
            } else {
                it = m_staged.erase(it);
            }
        }
    }

    /// `LoadTile` **回退到同步生成**的累计次数（命中缓存 = 0；线程池不可用时回退属预期）。
    /// 观测用：10km 实测要求该值稳定为 0（有 worker 时不得在渲染帧内同步生成）。
    [[nodiscard]] std::size_t SyncFallbackCount() const noexcept { return m_syncFallbackCount; }

    /// 安装一份**只重网格**（relod）的结果：高度不变，只换该 tile 的网格。
    /// 要求 `coord` 仍常驻且 `mesh.lodLevel == lodLevel`；否则返回 `false`（已卸载 / LOD 不符 ⇒ 丢弃，不静默使用陈旧数据）。
    bool InstallRemeshedMesh(const TileCoord& coord, int lodLevel, TerrainTileMesh mesh);

    /// **释放该 tile 的 CPU 侧网格缓冲**（`clear()` + `shrink_to_fit()`），但**保留**：
    /// ① 高度数据（重网格要用）；② `meshEmpty`（接管判据）；③ `lodLevel` / `verticesPerSide` / `coord`。
    ///
    /// 用途（W7-S3b）：10km 常驻 4489 个 tile 时 `m_meshes` 的 CPU 侧 `MeshData` 约 160 MB（超 ADR 0008 的
    /// CPU 预算）；GPU 上传完成后这些缓冲**不再被渲染使用**，可释放（目标是把地形相关的 CPU 常驻从 ≈160 MB
    /// 压到 ≈40 MB = 只剩高度 + 标记）。
    /// 释放后仍可重网格：`MeshTile` 从**高度**重建 ⇒ relod / 笔刷 / 爆破路径天然可用。
    void ReleaseTileMeshCpu(int tileX, int tileZ);

    // ---- 列访问（笔刷 / 存档用）----

    /// 读取世界列 `(worldX, worldZ)` 的当前高度。
    /// 返回 false 表示该列未被任何已加载 tile 持有。
    ///
    /// **成本 O(1)**（T79①）：由整数除法算出**最多 4 个**候选 tile 再查表，不遍历全部已加载 tile
    ///（`QueryHeight` 是每帧热路径 —— 相机避障 / 弹道 / 材质派生）。
    /// **共享边界列**（tile 原点与原点 + 64 那两列）会被两个 tile 同时持有，二者内容恒等（红线 12）。
    [[nodiscard]] bool ReadColumnHeight(int worldX, int worldZ, Height& outHeight) const noexcept;

    /// 写入世界列高度：更新**所有**含该列的已加载 tile（含共享边界层），
    /// 并把被改动的 tile 坐标追加到 `dirtyOut`（可能重复，调用方负责去重）。
    /// 传入值会被钳制到世界垂直范围（ADR 0008）。
    /// **成本 O(1)**（T79①，候选 tile 同 `ReadColumnHeight`）。
    void WriteColumnHeight(int worldX, int worldZ, Height height, std::vector<TileCoord>& dirtyOut);

    // ---- 高度场差量（存档；[ADR 0037](../../docs/adr/0037-world-state-save-v2-and-terrain-persistence.md) 决策二~四）----

    /// 一个 tile 的**高度差量导出项**：`entries` 按 `columnIndex` **严格升序**
    /// （可直接喂 `EncodeHeightDirtyTile`；列号 0..4095 = 行主序，**只含本 tile 拥有的 64² 列**）。
    ///
    /// 形态与 `.voxr` 的 `HeightDirtyEntry` **同源** —— ADR 0037 决策四要求"差量的内存表示复用既有脏标记、
    /// 不新开第二条记账"，故直接复用该结构，落盘时零转换。
    struct HeightEditTile {
        TileCoord                     coord {};
        std::vector<HeightDirtyEntry> entries;
    };

    /// 导出**全部已编辑 tile** 的高度差量（按 `TileCoord` 升序 ⇒ 写盘顺序确定，红线 7）。
    ///
    /// 差量 = `当前高度 − 生成结果高度`（皆 1/16 格，`HeightDirtyEntry::heightDelta`）。
    /// 生成结果由**纯函数重算**（与 `GenerateTile` **同一数据路径**，含预制来源）—— 故不额外占内存、
    /// 不引入第二条记账；未改动过的 tile **不出现**在结果里（ADR 0037 判据②）。
    ///
    /// 已卸载但记录为脏的 tile **跳过**（其差量应在卸载前落盘，见决策四 / 阶段计划 S4）。
    [[nodiscard]] std::vector<HeightEditTile> ExportHeightEdits() const;

    /// **单 tile** 的高度差量（未编辑 / 与生成结果无差异 / 未常驻 ⇒ 空）。口径与 `ExportHeightEdits` 逐字相同。
    ///
    /// 用途（S4）：① 单元**卸载前**采集（"不静默丢改动"）；② flush 时逐个常驻脏 tile 采集（避免一次遍历全部）。
    [[nodiscard]] std::vector<HeightDirtyEntry> ExportTileEdits(const TileCoord& coord) const;

    /// **已知被编辑过的 tile 坐标**（升序；含已被读档叠加过的）。供 S4 的 flush 遍历。
    ///
    /// 注意语义：它是"曾与生成结果不同"的集合，**不**保证该 tile 现在常驻、也**不**保证当前仍有差异
    /// （编辑回原值后差量为空 ⇒ 采集结果为空，`SetChunk` 会删除该块）。
    [[nodiscard]] std::vector<TileCoord> EditedTiles() const;

    /// 叠加一个 tile 的高度差量（**读档路径**；必须在 `GenerateTile` **之后**调用 —— ADR 0037 决策三禁止颠倒）。
    ///
    /// 逐条按**世界列**写入（复用 `WriteColumnHeight` 的"写进全部持有该列的 tile"语义 ⇒ 共享边界列不裂缝，红线 12）；
    /// 新增值 = `生成结果 + 差量` ⇒ **幂等**（重复叠加结果相同）。被改动的 tile 追加到 `dirtyOut` 并标记为已编辑。
    /// `coord` 未常驻 ⇒ 无操作（等它被流式建出时再叠加，见决策三）。
    void ApplyHeightEdits(const TileCoord& coord, const std::vector<HeightDirtyEntry>& entries,
                          std::vector<TileCoord>& dirtyOut);

    /// 该 tile 是否**被编辑过**（含读档叠加）。供流式"脏单元不卸 / 卸前先落盘"判定（ADR 0037 决策四）。
    [[nodiscard]] bool IsTileEdited(const TileCoord& coord) const noexcept {
        return m_editedTiles.find(coord) != m_editedTiles.end();
    }

    /// 已知被编辑的 tile 数（观测）。
    [[nodiscard]] std::size_t EditedTileCount() const noexcept { return m_editedTiles.size(); }

    /// **地形编辑序号**：`WriteColumnHeight` 每写进一个持有该列的 tile 就 `+1`（单调递增）。
    ///
    /// 用途（S4）：调用方以 **O(1)** 判定"自上次落盘以来地形有没有变过" ⇒ 没变就跳过
    /// "重算全部差量"这一步（`ExportTileEdits` 要为每个脏 tile 重算一次生成结果，代价不低）。
    /// 语义只保证"**有改动 ⇒ 序号必变**"（不保证"没变 ⇒ 序号不变"的反向严格性，也不保证步长）。
    [[nodiscard]] std::uint64_t EditSerial() const noexcept { return m_editSerial; }

    // ---- 重网格 ----

    /// 只重网格 `dirty` 中列出的 tile（按坐标去重），返回实际重网格的 tile 数。
    /// 未加载的 tile 跳过；**不触碰**其余 tile（red line：禁止整世界重网格）。
    [[nodiscard]] std::size_t RemeshDirtyTiles(const std::vector<TileCoord>& dirty);

    // ---- ITerrainQuery ----

    [[nodiscard]] bool QueryHeight(float worldX, float worldZ, float& outHeight) const override;

    /// 该列在**可挖体积**中使用的材质槽位（[ADR 0014](../../docs/adr/0014-voxel-material-index.md)）。
    ///
    /// 取该列地表 splat 的**主槽位**（权重最大者），再按材质表的 `subsurfaceSlot` 做
    /// **表层 → 次表层**映射（草 / 沙 ⇒ 土；岩 / 土保持自身）⇒ "在草地上挖坑看到土、在山体里挖洞看到岩"。
    /// 坡度用中心差分求（与地表着色同一口径）。返回 `false` 表示该列没有地形（未加载）。
    [[nodiscard]] bool QueryDigMaterialSlot(float worldX, float worldZ, std::uint8_t& outSlot) const;
    [[nodiscard]] bool QueryObstruction(const glm::vec3& from, const glm::vec3& to, float& outSafeT) const override;

    /// `ITerrainQuery::IsSolid`：**只按地表高度场**判定（`y <= 地表高度` 即为实心）。
    ///
    /// 注意：本类**不知道可挖体积**（分层边界）。相机实际使用的是游戏层的组合查询
    /// （`game/main.cpp` 的 `GameCameraQuery`：区域内以体积为准）—— 只用地表会把相机顶出洞外。
    [[nodiscard]] bool IsSolid(const glm::vec3& point) const override;

    /// 已加载 tile 中的**最高地表高度**（格）。无 tile 时返回 0。
    /// 供上层推导阴影投射体高度（缺陷 1）：`最高地表高度 − 渲染原点高度` 即最高投射体相对原点的高度。
    /// 每帧调用，成本为 **O(tile 数)**（各 tile 的顶点最大值在生成 / 重网格时已缓存，
    /// 见 `TerrainTile::maxSurfaceBlocks`）；不分配、不读全局。
    [[nodiscard]] float MaxSurfaceHeightBlocks() const noexcept;

    [[nodiscard]] std::uint64_t Seed() const noexcept { return m_seed; }

    /// 本世界所用的材质表（启动期加载的**同一份**）。
    /// 调用方据此构建 GPU uniform 块（`BuildMaterialUniform`），保证 CPU 与 GPU 参数同源（ADR 0009）。
    [[nodiscard]] const TerrainMaterialTable& Materials() const noexcept { return m_materials; }

private:
    /// 按当前世界定义（种子 / 生成参数 / 预设编辑 / **数据来源**）**重算**一个 tile 的初始高度（纯函数路径）。
    ///
    /// `GenerateTile` / `ExportHeightEdits` / `ApplyHeightEdits` 三条路径**共用同一实现** ⇒
    /// 存档差量的**基准**与生成结果必然同源（否则差量会随"谁先算"而漂移）。
    [[nodiscard]] TerrainTile BuildPristineTile(int tileX, int tileZ) const;

    std::uint64_t         m_seed = 0;
    TerrainMaterialTable  m_materials;
    TerrainNoiseGenerator m_noise;
    std::vector<MapEdit>  m_mapEdits;  ///< 预设地图的地形编辑（T11）；空表示纯噪声世界

    /// 层间交接过滤器（T8，非拥有；`nullptr` = 地表网格全覆盖）。
    const ITerrainQuadFilter* m_quadFilter = nullptr;

    /// 地表数据来源（V4，非拥有；`nullptr` = 程序化生成，即引入本能力之前的路径）。
    const ITerrainTileSource* m_tileSource = nullptr;

    std::map<TileCoord, TerrainTile>     m_tiles;
    std::map<TileCoord, TerrainTileMesh> m_meshes;

    /// **worker 预取缓存**（W7-S3b）：已生成、未过滤、尚未安装的 tile（`LoadTile` 命中即安装）。
    std::map<TileCoord, StagedTerrainTile> m_staged;

    /// `LoadTile` 回退到同步生成的累计次数（观测；有 worker 时必须稳定为 0）。
    std::size_t m_syncFallbackCount = 0;

    /// **已编辑（当前高度 ≠ 生成结果）的 tile 集合** —— 高度场差量的**内存表示**（ADR 0037 决策四）。
    ///
    /// `std::set` ⇒ 遍历天然按 `TileCoord` 升序（导出 / 写盘顺序确定，红线 7）；只在 `WriteColumnHeight` /
    /// `ApplyHeightEdits` 写入时插入，**不新开第二条记账**（与既有"脏 tile"是同一份事实）。
    std::set<TileCoord> m_editedTiles;

    /// **地形编辑序号**（语义见 `EditSerial()`）：每写进一个 tile 就 +1；`EditSerial()` 是它的只读视图。
    std::uint64_t m_editSerial = 0;
};

}  // namespace vx
