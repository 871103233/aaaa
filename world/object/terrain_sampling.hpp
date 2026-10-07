#pragma once

#include "generation/terrain_params.hpp"   // LandformKind / ClassifyLandform
#include "object/object_placement_rule.hpp"  // PlacementSample

#include <cmath>

namespace vx {

namespace terrain_sampling_detail {
constexpr double kPi = 3.14159265358979323846;
}

/// 由**高度梯度**求坡度（纯函数；单位：度）。
///
/// 坡度 = `atan(|∇h|)`，其中 `∇h = (dh/dx, dh/dz)` 为该点高度场梯度（无量纲 ⇒ `dh/dx = 1` 即 45°）。
/// `dhdx <= 0 && dhdz <= 0` 的平地上返回 `0`；值域 `[0, 90)`。
[[nodiscard]] inline float SlopeDegreesFromGradient(float dhdx, float dhdz) noexcept {
    const double gradient = std::sqrt(static_cast<double>(dhdx) * static_cast<double>(dhdx) +
                                      static_cast<double>(dhdz) * static_cast<double>(dhdz));
    return static_cast<float>(std::atan(gradient) * 180.0 / terrain_sampling_detail::kPi);
}

/// 由**中心列与四邻高度**求坡度（纯函数；**中心差分**，邻间距 = `2 × halfStepBlocks`）。
///
/// `halfStepBlocks` 是"到每个邻点的距离"（格）；`<= 0` ⇒ 无法求差分，返回 `0`（调用方保证为正）。
[[nodiscard]] inline float SlopeDegreesFromNeighbors(float hMinusX, float hPlusX, float hMinusZ, float hPlusZ,
                                                     float halfStepBlocks) noexcept {
    if (!(halfStepBlocks > 0.0F)) {
        return 0.0F;
    }
    const float step = 2.0F * halfStepBlocks;
    return SlopeDegreesFromGradient((hPlusX - hMinusX) / step, (hPlusZ - hMinusZ) / step);
}

/// **在 `(x, z)` 处采出一个 `PlacementSample`**（纯函数；[ADR 0033](../../docs/adr/0033-world-content-placement-and-streaming.md) 决策一）。
///
/// 把"地形 / 气候怎么读"抽象成四个回调 ⇒ 本函数**不依赖 `TerrainWorld` / 噪声生成器**，可单测：
///   - `heightAt(x, z)` ⇒ 该世界列的地表高度（**格**）；
///   - `landformAt(x, z)` ⇒ 该世界列的地貌（`LandformKind`）；
///   - `temperatureAt(x, z)` / `humidityAt(x, z)` ⇒ **气候**（`[0, 1]`，V0.6 C7）。
///
/// 坡度用**中心差分**（四邻，间距 `halfStepBlocks`）。五个字段共同构成 `IsPlacementAllowed` 的输入。
template <typename HeightFn, typename LandformFn, typename TemperatureFn, typename HumidityFn>
[[nodiscard]] PlacementSample SamplePlacement(float x, float z, float halfStepBlocks, HeightFn&& heightAt,
                                              LandformFn&& landformAt, TemperatureFn&& temperatureAt,
                                              HumidityFn&& humidityAt) {
    PlacementSample sample;
    sample.heightBlocks = static_cast<float>(heightAt(x, z));
    sample.slopeDegrees = SlopeDegreesFromNeighbors(static_cast<float>(heightAt(x - halfStepBlocks, z)),
                                                    static_cast<float>(heightAt(x + halfStepBlocks, z)),
                                                    static_cast<float>(heightAt(x, z - halfStepBlocks)),
                                                    static_cast<float>(heightAt(x, z + halfStepBlocks)),
                                                    halfStepBlocks);
    sample.landform     = landformAt(x, z);
    sample.temperature  = static_cast<float>(temperatureAt(x, z));
    sample.humidity     = static_cast<float>(humidityAt(x, z));
    return sample;
}

}  // namespace vx
