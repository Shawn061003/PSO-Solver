#include "pso/config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace pso {
namespace {

// 去除字符串首尾空白。使用 unsigned char 避免非 ASCII 字节传给 isspace 时产生未定义行为。
std::string trim(std::string value) {
    const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

// 删除 YAML 行尾注释，但引号内部的 # 仍然属于字符串内容。
// 本项目只需要单行标量，因此无需处理 YAML 的多行块字符串。
std::string strip_comment(const std::string& line) {
    bool single_quote = false;
    bool double_quote = false;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const char ch = line[index];
        if (ch == '\'' && !double_quote) {
            single_quote = !single_quote;
        } else if (ch == '"' && !single_quote) {
            double_quote = !double_quote;
        } else if (ch == '#' && !single_quote && !double_quote) {
            return line.substr(0, index);
        }
    }
    return line;
}

// 查找当前映射行真正的冒号分隔符。
// 引号中的冒号以及行内数组内部的冒号不能被误判为 key/value 分界。
std::size_t find_mapping_colon(const std::string& text) {
    bool single_quote = false;
    bool double_quote = false;
    int bracket_depth = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char ch = text[index];
        if (ch == '\'' && !double_quote) {
            single_quote = !single_quote;
        } else if (ch == '"' && !single_quote) {
            double_quote = !double_quote;
        } else if (!single_quote && !double_quote) {
            bracket_depth += ch == '[' ? 1 : 0;
            bracket_depth -= ch == ']' ? 1 : 0;
            if (ch == ':' && bracket_depth == 0) {
                return index;
            }
        }
    }
    return std::string::npos;
}

// 数值和布尔值通常没有引号；字符串允许使用一对单引号或双引号。
std::string unquote(std::string value) {
    value = trim(std::move(value));
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

// 所有类型读取函数共用的缺失键检查，错误信息中保留完整点分键方便定位 YAML。
const std::string& require_value(
    const std::unordered_map<std::string, std::string>& values,
    const std::string& key) {
    const auto iterator = values.find(key);
    if (iterator == values.end()) {
        throw std::runtime_error("Missing YAML key: " + key);
    }
    return iterator->second;
}

}  // namespace

YamlConfig YamlConfig::load(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Cannot open YAML config: " + path.string());
    }

    YamlConfig config;

    // sections 保存当前缩进路径，例如 [(0,"solver"), (2,"advanced")]。
    // 读到叶子键时，会拼成 solver.advanced.xxx 存入 values_。
    std::vector<std::pair<std::size_t, std::string>> sections;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;

        // YAML 允许复杂的缩进规则，但本读取器为了避免编辑器显示差异，明确禁止 Tab。
        if (line.find('\t') != std::string::npos) {
            throw std::runtime_error(
                "Tabs are not allowed in YAML indentation at line " +
                std::to_string(line_number));
        }
        // 先去注释，再忽略空行，使后续逻辑只处理真正的映射行。
        line = strip_comment(line);
        if (trim(line).empty()) {
            continue;
        }

        const auto first_non_space = line.find_first_not_of(' ');
        const std::size_t indent = first_non_space == std::string::npos ? 0 : first_non_space;
        const std::string content = line.substr(indent);

        // 每个有效行必须是 section: 或 key: value。
        const std::size_t colon = find_mapping_colon(content);
        if (colon == std::string::npos) {
            throw std::runtime_error(
                "Expected 'key: value' at YAML line " + std::to_string(line_number));
        }

        const std::string key = trim(content.substr(0, colon));
        const std::string value = trim(content.substr(colon + 1));
        if (key.empty()) {
            throw std::runtime_error("Empty YAML key at line " + std::to_string(line_number));
        }

        // 当缩进回退到同级或更外层时，弹出已经结束的 section。
        while (!sections.empty() && indent <= sections.back().first) {
            sections.pop_back();
        }

        // 将当前 section 路径和叶子 key 拼成稳定的点分键。
        std::string full_key;
        for (const auto& [_, section] : sections) {
            if (!full_key.empty()) {
                full_key += '.';
            }
            full_key += section;
        }
        if (!full_key.empty()) {
            full_key += '.';
        }
        full_key += key;

        if (value.empty()) {
            // 没有 value 的行被视为父级映射，只更新缩进栈，不写入 values_。
            sections.emplace_back(indent, key);
            continue;
        }

        // 重复键往往意味着复制配置时遗漏修改，直接报错比静默覆盖更安全。
        if (!config.values_.emplace(full_key, value).second) {
            throw std::runtime_error("Duplicate YAML key: " + full_key);
        }
    }
    return config;
}

bool YamlConfig::contains(const std::string& key) const {
    return values_.contains(key);
}

std::string YamlConfig::get_string(const std::string& key) const {
    return unquote(require_value(values_, key));
}

double YamlConfig::get_double(const std::string& key) const {
    const std::string value = unquote(require_value(values_, key));
    std::size_t consumed = 0;
    const double result = std::stod(value, &consumed);

    // stod 允许只转换前缀，例如 "1.0abc"；必须检查 consumed 才能做到严格类型校验。
    if (consumed != value.size()) {
        throw std::runtime_error("YAML key is not a number: " + key);
    }
    return result;
}

std::size_t YamlConfig::get_size(const std::string& key) const {
    // 数量类字段不允许负数，先按 uint64_t 读取，再转换成当前平台的 size_t。
    const auto result = get_uint64(key);
    return static_cast<std::size_t>(result);
}

std::uint64_t YamlConfig::get_uint64(const std::string& key) const {
    const std::string value = unquote(require_value(values_, key));
    std::size_t consumed = 0;
    const auto result = std::stoull(value, &consumed);
    if (consumed != value.size()) {
        throw std::runtime_error("YAML key is not an unsigned integer: " + key);
    }
    return result;
}

bool YamlConfig::get_bool(const std::string& key) const {
    std::string value = unquote(require_value(values_, key));
    // 大小写不敏感地接受 true/false，但不接受 0/1、yes/no，减少配置歧义。
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (value == "true") {
        return true;
    }
    if (value == "false") {
        return false;
    }
    throw std::runtime_error("YAML key is not true or false: " + key);
}

std::vector<double> YamlConfig::get_double_list(const std::string& key) const {
    std::string value = trim(require_value(values_, key));
    if (value.size() < 2 || value.front() != '[' || value.back() != ']') {
        throw std::runtime_error("YAML key is not an inline list: " + key);
    }
    // 去掉方括号后按逗号切分。本项目的数组元素仅允许普通数值，不支持嵌套数组。
    value = value.substr(1, value.size() - 2);
    std::vector<double> result;
    std::stringstream stream(value);
    std::string item;
    while (std::getline(stream, item, ',')) {
        item = trim(item);
        if (item.empty()) {
            throw std::runtime_error("Empty item in YAML list: " + key);
        }
        std::size_t consumed = 0;
        const double parsed = std::stod(item, &consumed);
        if (consumed != item.size()) {
            throw std::runtime_error("Non-numeric item in YAML list: " + key);
        }
        result.push_back(parsed);
    }
    return result;
}

PsoSettings load_pso_settings(const std::filesystem::path& path) {
    // 该函数定义了通用 PSO 的标准 YAML 模式。具体赛题的额外字段可由使用方再通过
    // YamlConfig::load(path) 读取，算法库无需了解速度、时间、坐标等业务含义。
    const YamlConfig yaml = YamlConfig::load(path);
    PsoSettings settings;
    settings.solver.swarm_size = yaml.get_size("solver.swarm_size");
    settings.solver.max_iterations = yaml.get_size("solver.max_iterations");
    settings.solver.inertia_start = yaml.get_double("solver.inertia_start");
    settings.solver.inertia_end = yaml.get_double("solver.inertia_end");
    settings.solver.cognitive = yaml.get_double("solver.cognitive");
    settings.solver.social = yaml.get_double("solver.social");
    settings.solver.velocity_limit_ratio =
        yaml.get_double("solver.velocity_limit_ratio");
    settings.solver.boundary_damping = yaml.get_double("solver.boundary_damping");
    settings.solver.tolerance = yaml.get_double("solver.tolerance");
    settings.solver.stall_iterations = yaml.get_size("solver.stall_iterations");
    settings.solver.seed = yaml.get_uint64("solver.seed");
    settings.solver.maximize = yaml.get_bool("solver.maximize");

    // lower[i]、upper[i] 与目标函数决策向量 x[i] 必须保持完全相同的顺序。
    const auto lower = yaml.get_double_list("bounds.lower");
    const auto upper = yaml.get_double_list("bounds.upper");
    if (lower.empty() || lower.size() != upper.size()) {
        throw std::runtime_error(
            "bounds.lower and bounds.upper must have the same non-zero length");
    }
    settings.bounds.reserve(lower.size());
    for (std::size_t index = 0; index < lower.size(); ++index) {
        // 在加载阶段检查每一维边界，错误会比进入优化器后更接近配置来源。
        if (lower[index] >= upper[index]) {
            throw std::runtime_error(
                "Each bounds.lower value must be smaller than bounds.upper");
        }
        settings.bounds.emplace_back(lower[index], upper[index]);
    }
    return settings;
}

}  // namespace pso
