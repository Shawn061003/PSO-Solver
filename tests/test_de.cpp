#include "de/de.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <string_view>

namespace {

std::string_view variant_name(de::Variant variant) {
    switch (variant) {
    case de::Variant::de:
        return "DE";
    case de::Variant::sade:
        return "SaDE";
    case de::Variant::jade:
        return "JADE";
    case de::Variant::shade:
        return "SHADE";
    case de::Variant::lshade:
        return "L-SHADE";
    }
    return "unknown";
}

}  // namespace

int main() {
    const optimization::Bounds bounds{{-5.0, 5.0}, {-5.0, 5.0}};
    const optimization::Objective sphere = [](const optimization::Vector& x) {
        return x[0] * x[0] + x[1] * x[1];
    };
    constexpr std::array variants{
        de::Variant::de,
        de::Variant::sade,
        de::Variant::jade,
        de::Variant::shade,
        de::Variant::lshade,
    };
    constexpr std::array crossovers{
        de::Crossover::binomial,
        de::Crossover::exponential,
    };

    // 每种 DE 变体都分别验证二项交叉和指数交叉。
    for (const de::Variant variant : variants) {
        for (const de::Crossover crossover : crossovers) {
            de::DeConfig config;
            config.variant = variant;
            config.common.max_generations = 220;
            config.common.stall_generations = 80;
            config.common.tolerance = 1e-10;
            config.common.seed = 100 + static_cast<unsigned>(variant) * 10 +
                                 static_cast<unsigned>(crossover);
            config.common.crossover = crossover;
            config.basic.population_size = 60;
            config.sade.population_size = 60;
            config.jade.population_size = 60;
            config.shade.population_size = 60;
            config.lshade.initial_population_size = 80;
            config.lshade.min_population_size = 4;

            const de::DifferentialEvolutionOptimizer optimizer(bounds, config);
            const optimization::Result result = optimizer.optimize(sphere);
            if (result.best_value >= 1e-5 || result.best_position.size() != 2 ||
                result.history.size() != result.iterations + 1) {
                std::cerr << variant_name(variant) << " failed with "
                          << (crossover == de::Crossover::binomial ? "binomial" : "exponential")
                          << " crossover: " << result.best_value << '\n';
                return 1;
            }
        }
    }

    // 基础 DE 的五种变异策略都必须可以独立选择并完成求解。
    constexpr std::array mutation_strategies{
        de::MutationStrategy::rand1,
        de::MutationStrategy::best1,
        de::MutationStrategy::current_to_best1,
        de::MutationStrategy::rand2,
        de::MutationStrategy::best2,
    };
    for (const de::MutationStrategy strategy : mutation_strategies) {
        de::DeConfig config;
        config.variant = de::Variant::de;
        config.common.max_generations = 220;
        config.common.stall_generations = 80;
        config.common.seed = 300 + static_cast<unsigned>(strategy);
        config.basic.population_size = 60;
        config.basic.mutation_strategy = strategy;
        const de::DifferentialEvolutionOptimizer optimizer(bounds, config);
        const optimization::Result result = optimizer.optimize(sphere);
        if (result.best_value >= 1e-5) {
            std::cerr << "Basic DE mutation strategy failed: "
                      << static_cast<unsigned>(strategy) << ", value="
                      << result.best_value << '\n';
            return 1;
        }
    }

    // 单独验证最大化方向，避免算法切换后误把较小值当作较优值。
    de::DeConfig maximize_config;
    maximize_config.variant = de::Variant::de;
    maximize_config.common.maximize = true;
    maximize_config.common.max_generations = 180;
    maximize_config.common.stall_generations = 60;
    maximize_config.common.seed = 500;
    maximize_config.basic.population_size = 60;
    const de::DifferentialEvolutionOptimizer maximize_optimizer(bounds, maximize_config);
    const auto maximize_result = maximize_optimizer.optimize(
        [](const optimization::Vector& x) {
            return -(x[0] * x[0] + x[1] * x[1]);
        });
    if (maximize_result.best_value <= -1e-5) {
        std::cerr << "Basic DE maximize direction failed: "
                  << maximize_result.best_value << '\n';
        return 1;
    }
    return 0;
}
