#pragma once

#include "optimization/types.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace es {

// 基础 ES 的环境选择方式。
enum class SelectionMode {
    comma, // (mu, lambda)-ES：下一代父代只从子代中选择，探索能力更强。
    plus,  // (mu + lambda)-ES：父代和子代共同竞争，具有精英保留。
};

// 多父代重组方式。
enum class Recombination {
    intermediate, // 中间重组：两个父代对应分量取平均值。
    discrete,     // 离散重组：每一维随机继承两个父代之一。
};

// ES 和 CMA-ES 共用的运行参数。
struct CommonConfig {
    // 最大进化代数；实际运行可能因停滞或数值条件提前结束。
    std::size_t max_generations{300};

    // 最优值改善超过该阈值才算有效改善。
    double tolerance{1e-8};

    // 连续多少代没有有效改善时提前停止。
    std::size_t stall_generations{80};

    // 固定随机种子便于复现实验。
    std::uint64_t seed{2026};

    // false 求最小值，true 求最大值。
    bool maximize{false};
};

// 基础各向同性高斯演化策略的参数。
struct EsConfig {
    CommonConfig common;

    // 每代保留的父代数量 mu，至少为 2。
    std::size_t parent_count{20};

    // 每代生成的子代数量 lambda；comma 模式下必须不小于 parent_count。
    std::size_t offspring_count{80};

    // 初始全局变异步长 sigma。算法在归一化 [0,1] 空间运行，因此它表示搜索区间比例。
    double initial_step_size{0.15};

    // sigma 的上下限，避免步长下溢或无限放大。
    double min_step_size{1e-8};
    double max_step_size{1.0};

    // comma 对应 (mu,lambda)-ES；plus 对应 (mu+lambda)-ES。
    SelectionMode selection_mode{SelectionMode::plus};

    // intermediate 为两父代均值；discrete 为各维随机选择父代。
    Recombination recombination{Recombination::intermediate};

    // 是否使用经典 1/5 成功规则自动调整 sigma。
    bool adapt_step_size{true};

    // 每隔多少代统计一次成功率并调整 sigma。
    std::size_t adaptation_interval{10};

    // 步长调整倍率，必须大于 1。成功率高于 1/5 时乘以它，否则除以它。
    double adaptation_factor{1.5};
};

// 标准全协方差 CMA-ES 参数。
struct CmaEsConfig {
    CommonConfig common;

    // 子代数量 lambda；设为 0 时自动取 4 + floor(3*ln(n))。
    std::size_t population_size{0};

    // 参与加权重组的优秀子代数量 mu；设为 0 时自动取 lambda/2。
    std::size_t parent_count{0};

    // 初始全局步长 sigma，占各维搜索区间的比例。
    double initial_step_size{0.25};

    // sigma 的数值保护范围。
    double min_step_size{1e-12};
    double max_step_size{2.0};

    // 步长累积路径学习率 c_sigma；0 表示使用标准自动公式。
    double c_sigma{0.0};

    // 步长阻尼 d_sigma；0 表示使用标准自动公式。
    double d_sigma{0.0};

    // 协方差累积路径学习率 c_c；0 表示使用标准自动公式。
    double c_c{0.0};

    // 协方差矩阵秩一更新学习率 c1；0 表示使用标准自动公式。
    double c1{0.0};

    // 协方差矩阵秩 mu 更新学习率 c_mu；0 表示使用标准自动公式。
    double c_mu{0.0};

    // 每隔多少代重新分解一次协方差矩阵；1 最稳妥，较大值可减少高维计算量。
    std::size_t eigen_update_period{1};

    // 协方差特征值下限，用于抑制浮点误差造成的非正定矩阵。
    double eigenvalue_floor{1e-20};

    // 协方差最大允许条件数；超过后提前停止，避免数值失稳。
    double max_condition_number{1e14};
};

// 基础 ES：支持 (mu,lambda)/(mu+lambda)、两种重组和 1/5 步长自适应。
class EvolutionStrategyOptimizer {
public:
    EvolutionStrategyOptimizer(optimization::Bounds bounds, EsConfig config = {});

    [[nodiscard]] optimization::Result optimize(
        const optimization::Objective& objective,
        const std::vector<optimization::Vector>& initial_positions = {},
        const optimization::Callback& callback = {}) const;

private:
    optimization::Bounds bounds_;
    EsConfig config_;
};

// 标准全协方差 CMA-ES。内部在归一化空间学习尺度和变量相关性。
class CmaEvolutionStrategyOptimizer {
public:
    CmaEvolutionStrategyOptimizer(optimization::Bounds bounds, CmaEsConfig config = {});

    // 提供热启动时，会先评估所有 initial_positions，并以其中最好者作为初始均值。
    [[nodiscard]] optimization::Result optimize(
        const optimization::Objective& objective,
        const std::vector<optimization::Vector>& initial_positions = {},
        const optimization::Callback& callback = {}) const;

private:
    optimization::Bounds bounds_;
    CmaEsConfig config_;
};

}  // namespace es
