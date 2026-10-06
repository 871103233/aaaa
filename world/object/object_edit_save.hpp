#pragma once

#include "object/object_layer.hpp"

#include <filesystem>

namespace vx {

/// 把**可编辑层**（`ObjectTable`）写成 TOML 文件（阶段 V0.5 的 E3；
/// [ADR 0032](../../docs/adr/0032-object-palette-and-placement-mode.md) 决策六）。
///
/// 写出的内容 = `schema_version` + `destructible_enabled` + `[[type]]` + `[[placement]]` + `[[remove]]` + `[[scatter]]`
/// —— 即"发布清单 → 编辑层"这一层的**全部增量**；读回走 `ObjectTable::LoadOverlayFromFile`（往返逐字段一致）。
///
/// 纪律（与 [ADR 0030](0030-instance-save-slot.md) 同口径）：**临时文件 + `rename` 原子替换**（崩溃不留半个文件）；
/// **覆盖式写入**，`toml++` **不保留注释** ⇒ 生成的编辑层**不带用户手写注释**（文件自带一行来源说明）。
/// 失败抛 `std::runtime_error`（不静默）。
void SaveObjectEditLayer(const std::filesystem::path& path, const ObjectTable& editLayer);

}  // namespace vx
