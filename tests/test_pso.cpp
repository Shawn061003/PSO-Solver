#include "pso/pso.hpp"

#include <cmath>
#include <iostream>

int main() {
    pso::PsoConfig config;
    config.swarm_size = 40;
    config.max_iterations = 140;
    config.stall_iterations = 50;
    config.seed = 7;

    const pso::Bounds bounds{{-5.0, 5.0}, {-5.0, 5.0}};
    const pso::ParticleSwarmOptimizer optimizer(bounds, config);
    const auto result = optimizer.optimize([](const pso::Vector& values) {
        return values[0] * values[0] + values[1] * values[1];
    });

    if (result.best_value >= 1e-6) {
        std::cerr << "Sphere function did not converge: " << result.best_value << '\n';
        return 1;
    }
    for (std::size_t index = 0; index < result.best_position.size(); ++index) {
        if (result.best_position[index] < bounds[index].first ||
            result.best_position[index] > bounds[index].second) {
            std::cerr << "Particle escaped its bounds\n";
            return 1;
        }
    }
    return 0;
}
