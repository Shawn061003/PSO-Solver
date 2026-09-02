#pragma once

#include <cstddef>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

namespace optimization {

// 所有优化算法共用的连续决策向量。x[i] 的物理意义由具体数学模型定义。
using Vector = std::vector<double>;

// 每一维决策变量的闭区间边界 [lower, upper]。
// bounds[i] 必须与目标函数中的 x[i] 一一对应，且 lower < upper。
using Bounds = std::vector<std::pair<double, double>>;

// 统一目标函数接口。求解器只比较返回值，不解释返回值的物理意义。
using Objective = std::function<double(const Vector&)>;

// 统一迭代回调：当前代数、当前全局最优值、当前全局最优位置。
using Callback = std::function<void(std::size_t, double, const Vector&)>;

// PSO 和全部 DE 变体共用的返回结构，便于调用方无缝切换算法。
struct Result {
    // 搜索结束时发现的最好决策向量。
    Vector best_position;

    // best_position 对应的目标函数值。
    double best_value{std::numeric_limits<double>::quiet_NaN()};

    // 实际执行的迭代代数或进化代数。
    std::size_t iterations{0};

    // 目标函数累计调用次数，适合用于公平比较不同算法的计算成本。
    std::size_t evaluations{0};

    // 是否因连续多代没有超过 tolerance 的改善而提前结束。
    bool converged{false};

    // optimize() 的墙钟运行时间，单位为秒。
    double elapsed_seconds{0.0};

    // history[0] 为初始化后的最优值，之后每个元素对应一代结束时的最优值。
    std::vector<double> history;
};

}  // namespace optimization
