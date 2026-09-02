#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

namespace pso {

// 决策向量。下标含义由具体赛题定义，例如 x[0] 可以表示速度、角度或时间。
using Vector = std::vector<double>;

// 每一维变量的闭区间边界：[lower, upper]。
// bounds[i] 必须与决策向量 x[i] 一一对应，且 lower < upper。
using Bounds = std::vector<std::pair<double, double>>;

// 目标函数统一返回一个 double。约束问题通常由调用方在函数内部加入罚值。
// 求解器不会解释返回值的物理意义，只根据 maximize 决定比较方向。
using Objective = std::function<double(const Vector&)>;

// 粒子群算法的全部通用超参数。
// 默认值适合先做小规模冒烟测试；正式求解应通过 YAML 显式给出并记录随机种子。
struct PsoConfig {
    // 粒子数量。数量越大，全局探索越充分，但每一代的目标函数调用次数也越多。
    std::size_t swarm_size{64};

    // 最大迭代代数；实际运行也可能因连续多代没有显著改进而提前结束。
    std::size_t max_iterations{150};

    // 惯性权重 w 从 start 线性变化到 end。
    // 较大的前期权重有利于全局探索，较小的后期权重有利于局部收敛。
    double inertia_start{0.9};
    double inertia_end{0.4};

    // 个体学习因子 c1：粒子回到自己历史最优位置的倾向。
    double cognitive{1.7};

    // 群体学习因子 c2：粒子靠近全局历史最优位置的倾向。
    double social{1.7};

    // 每一维最大速度占该维搜索区间宽度的比例，防止粒子一步跨越过大的区域。
    double velocity_limit_ratio{0.25};

    // 粒子撞到边界后速度反向并乘以该系数；0 表示贴边停止，1 表示完全弹回。
    double boundary_damping{0.5};

    // 只有全局最优值的改善量大于 tolerance，才视为一次“有效改善”。
    double tolerance{1e-8};

    // 连续多少代没有有效改善时提前停止。
    std::size_t stall_iterations{40};

    // 随机种子。相同配置、目标函数和编译环境下可用于复现实验。
    std::uint64_t seed{2026};

    // false 求最小值，true 求最大值。
    bool maximize{false};
};

// optimize() 的完整返回值。除最优解外，还保留计算成本和收敛轨迹，便于写论文。
struct PsoResult {
    // 搜索结束时发现的全局最优决策向量。
    Vector best_position;

    // best_position 对应的目标函数值。
    double best_value{std::numeric_limits<double>::quiet_NaN()};

    // 实际执行的迭代代数；提前停止时可能小于 max_iterations。
    std::size_t iterations{0};

    // 目标函数累计调用次数，可用于公平比较不同优化算法的计算成本。
    std::size_t evaluations{0};

    // 是否由“停滞代数达到阈值”触发提前收敛；跑满代数时为 false。
    bool converged{false};

    // optimize() 的墙钟时间，不包含调用方读取数据或输出结果的耗时。
    double elapsed_seconds{0.0};

    // history[0] 是初始化后的最优值，之后每个元素对应一代结束时的全局最优值。
    std::vector<double> history;
};

// 有界连续变量的经典全局粒子群优化器。
// 类对象本身不保存运行状态，因此同一个实例可以用相同配置重复调用 optimize()。
class ParticleSwarmOptimizer {
public:
    // 回调参数依次为：当前代数、当前全局最优值、当前全局最优位置。
    // 可用于打印进度或保存收敛曲线；不应在回调中修改优化器状态。
    using Callback = std::function<void(std::size_t, double, const Vector&)>;

    // 构造时完成参数和边界检查，尽早暴露 YAML 或建模阶段的配置错误。
    ParticleSwarmOptimizer(Bounds bounds, PsoConfig config = {});

    // 执行一次完整 PSO。
    //
    // initial_positions：可选的热启动粒子。其数量可以少于粒子总数，剩余粒子仍然
    // 在整个可行域内随机初始化；越界的热启动坐标会被裁剪到最近边界。
    // callback：可选的逐代观察函数。
    //
    // 注意：objective 可能会被调用很多次，应避免在其中重复读取大文件。
    [[nodiscard]] PsoResult optimize(
        const Objective& objective,
        const std::vector<Vector>& initial_positions = {},
        const Callback& callback = {}) const;

private:
    // bounds_ 的长度就是问题维数，也是每个粒子 position/velocity 的长度。
    Bounds bounds_;

    // 保存一份不可变的运行配置；optimize() 被声明为 const。
    PsoConfig config_;
};

}  // namespace pso
