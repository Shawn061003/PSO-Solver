#include "es/es.hpp"

#include <array>
#include <cmath>
#include <iostream>

namespace {

double shifted_sphere(const optimization::Vector& x) {
    const std::array optimum{1.2, -0.7, 2.0};
    double value = 0.0;
    for (std::size_t index = 0; index < optimum.size(); ++index) {
        const double difference = x[index] - optimum[index];
        value += difference * difference;
    }
    return value;
}

double rotated_ellipsoid(const optimization::Vector& x) {
    const double first = x[0] - 1.2;
    const double second = x[1] + 0.7;
    const double third = x[2] - 2.0;
    const double rotated_first = first + second;
    const double rotated_second = first - second;
    return 100.0 * rotated_first * rotated_first +
           rotated_second * rotated_second +
           0.5 * third * third;
}

}  // namespace

int main() {
    const optimization::Bounds bounds{
        {-5.0, 5.0},
        {-5.0, 5.0},
        {-5.0, 5.0},
    };

    // 覆盖 plus/intermediate 与 comma/discrete 两组基础 ES 组合。
    for (const auto [selection, recombination] : std::array{
             std::pair{es::SelectionMode::plus, es::Recombination::intermediate},
             std::pair{es::SelectionMode::comma, es::Recombination::discrete}}) {
        es::EsConfig config;
        config.common.max_generations = 450;
        config.common.stall_generations = 160;
        config.common.tolerance = 1e-10;
        config.common.seed = selection == es::SelectionMode::plus ? 610 : 620;
        config.parent_count = 24;
        config.offspring_count = 120;
        config.initial_step_size = 0.18;
        config.adaptation_interval = 8;
        config.adaptation_factor = 1.25;
        config.selection_mode = selection;
        config.recombination = recombination;

        const es::EvolutionStrategyOptimizer optimizer(bounds, config);
        const optimization::Result result = optimizer.optimize(shifted_sphere);
        if (result.best_value >= 1e-4 || result.best_position.size() != 3 ||
            result.history.size() != result.iterations + 1) {
            std::cerr << "Basic ES failed: " << result.best_value << '\n';
            return 1;
        }
    }

    es::CmaEsConfig cma_config;
    cma_config.common.max_generations = 500;
    cma_config.common.stall_generations = 180;
    cma_config.common.tolerance = 1e-12;
    cma_config.common.seed = 700;
    cma_config.initial_step_size = 0.20;
    const es::CmaEvolutionStrategyOptimizer cma_optimizer(bounds, cma_config);
    const optimization::Result cma_result = cma_optimizer.optimize(rotated_ellipsoid);
    if (cma_result.best_value >= 1e-7 || cma_result.best_position.size() != 3 ||
        cma_result.history.size() != cma_result.iterations + 1) {
        std::cerr << "CMA-ES failed: " << cma_result.best_value << '\n';
        return 1;
    }
    return 0;
}
