#pragma once

#include "de/de.hpp"
#include "es/es.hpp"
#include "optimization/types.hpp"
#include "pso/config.hpp"
#include "pso/pso.hpp"

#include <filesystem>
#include <string_view>
#include <vector>

namespace optimization {

// 统一入口支持的全部算法。
// 配置文件中分别写作 pso/de/sade/jade/shade/lshade/es/cmaes。
enum class Method {
    pso,
    de,
    sade,
    jade,
    shade,
    lshade,
    es,
    cmaes,
};

// 从 YAML 读取后的完整求解设置。只有 method 选中的算法参数会参与计算。
struct SolverSettings {
    Method method{Method::pso};
    Bounds bounds;
    pso::PsoConfig pso;
    de::DeConfig de;
    es::EsConfig es;
    es::CmaEsConfig cmaes;
};

// 将算法名称转换为枚举；名称不区分大小写，L-SHADE 也接受 "l-shade"。
[[nodiscard]] Method method_from_string(std::string_view name);

// 返回稳定的小写算法名，适合打印日志和保存结果。
[[nodiscard]] std::string_view method_name(Method method);

// 从指定 YAML 加载统一配置。solver.method 决定读取哪个算法的专用参数区。
[[nodiscard]] SolverSettings load_solver_settings(const std::filesystem::path& path);

// 推荐给比赛代码使用的统一门面。
// 更换算法时目标函数、结果类型和调用代码都不变，只需修改 YAML 的 solver.method。
class Solver {
public:
    explicit Solver(SolverSettings settings);

    [[nodiscard]] Result optimize(
        const Objective& objective,
        const std::vector<Vector>& initial_positions = {},
        const Callback& callback = {}) const;

    [[nodiscard]] Method method() const noexcept;

private:
    SolverSettings settings_;
};

}  // namespace optimization
