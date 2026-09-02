#pragma once

#include "optimization/types.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace de {

// 本实现提供的五种差分进化算法。
enum class Variant {
    de,      // 基础 DE/rand/1/bin。
    sade,    // 自适应选择变异策略和交叉率的 SaDE。
    jade,    // current-to-pbest/1 + 外部档案 + 参数自适应。
    shade,   // 使用历史记忆数组自适应 F 和 CR。
    lshade,  // SHADE 加上线性种群规模缩减。
};

// DE 试验个体的交叉方式。
enum class Crossover {
    binomial,    // 二项交叉：各维独立按 CR 决定，且强制至少选择一个变异分量。
    exponential, // 指数交叉：从随机维度开始，连续循环复制一段变异分量。
};

// 基础 DE 可以独立选择的经典变异策略。
enum class MutationStrategy {
    rand1,           // DE/rand/1：随机基向量 + 1 个差分向量，探索性较强。
    best1,           // DE/best/1：当前最优向量 + 1 个差分向量，收敛较快。
    current_to_best1,// DE/current-to-best/1：当前个体同时向最优个体和差分方向移动。
    rand2,           // DE/rand/2：随机基向量 + 2 个差分向量，多样性较强。
    best2,           // DE/best/2：当前最优向量 + 2 个差分向量。
};

// 五种 DE 共用的停止、随机数和优化方向参数。
struct CommonConfig {
    // 最大进化代数。实际运行可能因为停滞准则提前结束。
    std::size_t max_generations{300};

    // 只有最优值改善量大于 tolerance 才算有效改善。
    double tolerance{1e-8};

    // 连续多少代没有有效改善时提前停止。
    std::size_t stall_generations{80};

    // 随机种子；固定种子有利于复现实验和比较算法。
    std::uint64_t seed{2026};

    // false 表示求最小值，true 表示求最大值。
    bool maximize{false};

    // 所有 DE 变体使用的交叉方式，可选择二项交叉或指数交叉。
    Crossover crossover{Crossover::binomial};
};

// 基础 DE/rand/1/bin 的专用参数。
struct BasicConfig {
    // 固定种群数量。基础 DE/rand/1 至少需要 4 个个体，通常建议为维数的 5～10 倍。
    std::size_t population_size{80};

    // 差分缩放因子 F，控制差分向量步长，常用范围 0.4～0.9。
    double differential_weight{0.5};

    // 交叉概率 CR；对二项交叉表示各维选中概率，对指数交叉影响连续片段长度。
    double crossover_rate{0.9};

    // 基础 DE 使用的变异策略；SaDE、JADE、SHADE、L-SHADE 使用各自固定的自适应策略。
    MutationStrategy mutation_strategy{MutationStrategy::rand1};
};

// SaDE 的专用参数。本实现使用四种经典变异策略并按成功率自适应选择。
struct SadeConfig {
    // 固定种群数量；四策略中 rand/2 需要至少 6 个个体。
    std::size_t population_size{80};

    // 每隔多少代根据各策略成功/失败次数更新策略选择概率和 CR 均值。
    std::size_t learning_period{50};

    // F 的正态采样均值和标准差；采样结果会限制在 (0, 1]。
    double differential_weight_mean{0.5};
    double differential_weight_stddev{0.3};

    // 各策略初始 CR 均值，以及 CR 正态采样标准差。
    double initial_crossover_mean{0.5};
    double crossover_stddev{0.1};
};

// JADE 的专用参数。
struct JadeConfig {
    // 固定种群数量；current-to-pbest/1 至少需要 4 个个体。
    std::size_t population_size{80};

    // 从排名前 p_best_rate 的个体中随机选择 pbest，典型值为 0.05～0.20。
    double p_best_rate{0.1};

    // 参数均值的学习率 c；越大越重视最近一代的成功参数。
    double adaptation_rate{0.1};

    // F 和 CR 的初始均值，之后由成功试验个体自动更新。
    double initial_f_mean{0.5};
    double initial_cr_mean{0.5};
};

// SHADE 的专用参数。
struct ShadeConfig {
    // 固定种群数量。
    std::size_t population_size{100};

    // pbest 候选比例。
    double p_best_rate{0.1};

    // 成功参数历史记忆数组长度 H，常用值为 5～20。
    std::size_t memory_size{6};

    // F、CR 历史记忆的初始值。
    double initial_f_memory{0.5};
    double initial_cr_memory{0.5};
};

// L-SHADE 的专用参数。其参数记忆机制与 SHADE 相同，但种群规模逐代线性减小。
struct LshadeConfig {
    // 初始种群数量，通常应明显大于 min_population_size。
    std::size_t initial_population_size{120};

    // 搜索结束附近保留的最小种群数量；current-to-pbest/1 至少需要 4。
    std::size_t min_population_size{4};

    // pbest 候选比例。
    double p_best_rate{0.1};

    // 成功参数历史记忆数组长度及初始值。
    std::size_t memory_size{6};
    double initial_f_memory{0.5};
    double initial_cr_memory{0.5};
};

// 直接调用 DE 系列时使用的完整配置。只有 variant 对应的专用参数会生效。
struct DeConfig {
    Variant variant{Variant::de};
    CommonConfig common;
    BasicConfig basic;
    SadeConfig sade;
    JadeConfig jade;
    ShadeConfig shade;
    LshadeConfig lshade;
};

// 统一 DE 优化器。通过 config.variant 选择具体实现，返回值与 PSO 完全相同。
class DifferentialEvolutionOptimizer {
public:
    DifferentialEvolutionOptimizer(optimization::Bounds bounds, DeConfig config = {});

    // initial_positions 可用于热启动；不足种群数量的部分仍然随机初始化。
    // callback 在每一代结束后调用，可用于打印或记录收敛曲线。
    [[nodiscard]] optimization::Result optimize(
        const optimization::Objective& objective,
        const std::vector<optimization::Vector>& initial_positions = {},
        const optimization::Callback& callback = {}) const;

private:
    optimization::Bounds bounds_;
    DeConfig config_;
};

}  // namespace de
