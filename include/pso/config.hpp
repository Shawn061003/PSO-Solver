#pragma once

#include "pso/pso.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace pso {

// 一个轻量、零第三方依赖的 YAML 子集读取器。
//
// 读取时会把缩进映射展平成点分键，例如：
//   solver:
//     swarm_size: 64
// 会保存为 "solver.swarm_size" -> "64"。
// 该类适合数模比赛中的数值配置，不打算覆盖锚点、多行字符串等完整 YAML 语法。
class YamlConfig {
public:
    // 从指定路径完整加载配置。文件不存在、缩进含 Tab、键重复或语法错误时抛异常。
    static YamlConfig load(const std::filesystem::path& path);

    // 查询展平后的键是否存在。可用于实现使用方自己的可选配置项。
    [[nodiscard]] bool contains(const std::string& key) const;

    // 以下 get_* 按目标类型严格转换；键缺失或类型不匹配时抛 std::runtime_error。
    [[nodiscard]] std::string get_string(const std::string& key) const;
    [[nodiscard]] double get_double(const std::string& key) const;
    [[nodiscard]] std::size_t get_size(const std::string& key) const;
    [[nodiscard]] std::uint64_t get_uint64(const std::string& key) const;
    [[nodiscard]] bool get_bool(const std::string& key) const;

    // 读取形如 [1.0, 2.0, 3.0] 的行内数值数组。
    [[nodiscard]] std::vector<double> get_double_list(const std::string& key) const;

private:
    // 先保存原始字符串，再由 get_* 转换，能够给不同配置模式复用同一个解析器。
    std::unordered_map<std::string, std::string> values_;
};

// 运行通用 PSO 所需的最小配置集合：算法参数 + 每一维决策变量边界。
struct PsoSettings {
    PsoConfig solver;
    Bounds bounds;
};

// 读取固定模式的通用 PSO YAML：solver.*、bounds.lower、bounds.upper。
// lower/upper 必须长度相同、非空，并满足每一维 lower < upper。
[[nodiscard]] PsoSettings load_pso_settings(const std::filesystem::path& path);

}  // namespace pso
