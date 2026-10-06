#include "save/world_instance_save.hpp"

#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <toml++/toml.hpp>

namespace vx {
namespace {

[[nodiscard]] std::string Describe(const std::filesystem::path& path, const char* field) {
    return path.string() + ": 字段 [" + field + "] ";
}

[[nodiscard]] std::string ReadString(const toml::table& table, const std::filesystem::path& path, const char* field) {
    const std::optional<std::string> value = table[field].value<std::string>();
    if (!value.has_value()) {
        throw std::runtime_error(Describe(path, field) + "缺失或不是字符串");
    }
    return *value;
}

/// 解析 `seed`：**优先**十进制字符串（TOML 整数是**有符号 64 位**，而种子是 `u64` ⇒ 大于 `INT64_MAX` 的种子
/// 只能走字符串），也接受整数（取值 ≤ `INT64_MAX` 的手改 / 旧档）。
///
/// 严格性：字符串必须**全是十进制数字**（拒绝前导 `+` / `-` / 空白 / 尾随字符），否则报错而非静默截断。
[[nodiscard]] std::uint64_t ReadSeed(const toml::table& table, const std::filesystem::path& path) {
    const toml::node* node = table.get("seed");
    if (node == nullptr) {
        throw std::runtime_error(Describe(path, "seed") + "缺失");
    }
    if (const std::optional<std::string> text = node->value<std::string>(); text.has_value()) {
        if (text->empty() || text->find_first_not_of("0123456789") != std::string::npos) {
            throw std::runtime_error(Describe(path, "seed") + "必须只含十进制数字：\"" + *text + "\"");
        }
        try {
            return static_cast<std::uint64_t>(std::stoull(*text, nullptr, 10));
        } catch (const std::exception&) {
            throw std::runtime_error(Describe(path, "seed") + "超出无符号 64 位范围：\"" + *text + "\"");
        }
    }
    if (const std::optional<std::int64_t> integer = node->value<std::int64_t>(); integer.has_value()) {
        if (*integer < 0) {
            throw std::runtime_error(Describe(path, "seed") + "不能为负");
        }
        return static_cast<std::uint64_t>(*integer);
    }
    throw std::runtime_error(Describe(path, "seed") + "必须是字符串或整数");
}

/// v1 解析：`schema_version = 1` 的精确布局（见头文件注释与 `plans/v0.5.md` §1.13.1）。
[[nodiscard]] WorldInstanceSave ParseV1(const toml::table& document, const std::filesystem::path& path) {
    WorldInstanceSave save;

    const toml::node* instances = document.get("instance");
    if (instances == nullptr) {
        return save;  // 空存档合法（尚无任何秘境实例）
    }
    const toml::array* array = instances->as_array();
    if (array == nullptr) {
        throw std::runtime_error(path.string() + ": [instance] 必须是数组（写成 [[instance]]）");
    }

    save.instances.reserve(array->size());
    for (std::size_t index = 0; index < array->size(); ++index) {
        const toml::node*    node  = array->get(index);
        const toml::table*   entry = (node != nullptr) ? node->as_table() : nullptr;
        if (entry == nullptr) {
            throw std::runtime_error(path.string() + ": instance[" + std::to_string(index) + "] 必须是表");
        }

        SavedWorldInstance record;
        record.worldId = ReadString(*entry, path, "world_id");
        if (record.worldId.empty()) {
            throw std::runtime_error(path.string() + ": instance[" + std::to_string(index) + "] 的 [world_id] 不能为空");
        }
        record.seed = ReadSeed(*entry, path);

        const std::optional<std::int64_t> generation = (*entry)["generation"].value<std::int64_t>();
        if (!generation.has_value()) {
            throw std::runtime_error(Describe(path, "generation") + "缺失或不是整数");
        }
        if (*generation < 0 || *generation > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
            throw std::runtime_error(Describe(path, "generation") + "越界（应为 0 .. 4294967295）");
        }
        record.generation = static_cast<std::uint32_t>(*generation);

        save.instances.push_back(std::move(record));
    }
    return save;
}

/// **版本迁移钩子**（红线 8）：按 `schema_version` 分派。当前只有 v1。
///
/// 未来任何字段 / 布局变更 ⇒ 在此**逐级迁移**（vN → vN+1）并补迁移测试「旧档能被新版本正确读出」。
/// 现在遇到未知版本**一律拒绝**（**不静默误读**）—— 静默误读正是红线 8 要防的"存档静默损坏"。
[[nodiscard]] WorldInstanceSave ParseByVersion(std::int64_t version, const toml::table& document,
                                               const std::filesystem::path& path) {
    switch (version) {
        case kWorldInstanceSaveSchemaVersion:
            return ParseV1(document, path);
        default:
            throw std::runtime_error(path.string() + ": 不支持的秘境存档版本 " + std::to_string(version) +
                                     "（本程序支持 " + std::to_string(kWorldInstanceSaveSchemaVersion) +
                                     "）—— 该档可能由更高版本的存档写入；**不静默误读**，请保留该档待迁移");
    }
}

}  // namespace

WorldInstanceSave LoadWorldInstanceSave(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        return WorldInstanceSave {};  // 首次运行：无档 ⇒ 空存档，**不报错**（正常 roll 新种子）
    }

    toml::table document;
    try {
        document = toml::parse_file(path.string());
    } catch (const std::exception& parseError) {
        throw std::runtime_error("无法加载秘境存档 " + path.string() + ": " + parseError.what());
    }

    const std::optional<std::int64_t> version = document["schema_version"].value<std::int64_t>();
    if (!version.has_value()) {
        throw std::runtime_error(path.string() + ": 缺少 schema_version");
    }
    return ParseByVersion(*version, document, path);
}

void SaveWorldInstanceSave(const std::filesystem::path& path, const WorldInstanceSave& save) {
    toml::table document;
    (void)document.insert_or_assign("schema_version", static_cast<std::int64_t>(kWorldInstanceSaveSchemaVersion));

    toml::array instances;
    instances.reserve(save.instances.size());
    for (const SavedWorldInstance& record : save.instances) {
        toml::table entry;
        (void)entry.insert_or_assign("world_id", record.worldId);
        // 种子写**十进制字符串**：TOML 整数是有符号 64 位，而种子是 u64（可能 > INT64_MAX）⇒ 字符串才无损。
        (void)entry.insert_or_assign("seed", std::to_string(record.seed));
        (void)entry.insert_or_assign("generation", static_cast<std::int64_t>(record.generation));
        instances.push_back(std::move(entry));
    }
    (void)document.insert_or_assign("instance", std::move(instances));

    // 原子替换：先写临时文件，再 rename 覆盖 ⇒ 崩溃不会留下半个存档（save-and-serialization §6）。
    const std::filesystem::path tempPath = path.string() + ".tmp";
    {
        // binary 模式：禁止运行库做 CRLF 转换，保证落盘为纯 LF（仓库行尾约定）。
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("无法创建秘境存档临时文件 " + tempPath.string());
        }
        out << "# 秘境实例存档（V10 自动生成；单槽。删除本文件将从随机种子重新开始）\n";
        out << document;
        out.flush();
        if (!out) {
            throw std::runtime_error("写入秘境存档临时文件失败 " + tempPath.string());
        }
    }

    std::error_code renameError;
    std::filesystem::rename(tempPath, path, renameError);
    if (renameError) {
        std::error_code removeError;
        std::filesystem::remove(tempPath, removeError);  // 清理临时文件（尽力而为）
        throw std::runtime_error("秘境存档原子替换失败（" + path.string() + "）：" + renameError.message());
    }
}

}  // namespace vx
