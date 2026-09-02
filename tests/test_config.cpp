#include "pso/config.hpp"

#include <cmath>
#include <iostream>

int main() {
    try {
        const auto settings = pso::load_pso_settings(TEST_CONFIG_PATH);
        if (settings.solver.swarm_size != 64 || settings.solver.maximize) {
            std::cerr << "Incorrect integer value\n";
            return 1;
        }
        if (settings.bounds.size() != 2 ||
            std::abs(settings.bounds[0].first + 5.0) > 1e-12 ||
            std::abs(settings.bounds[1].second - 5.0) > 1e-12) {
            std::cerr << "Incorrect bounds\n";
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
