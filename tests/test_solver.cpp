#include "optimization/solver.hpp"

#include <exception>
#include <iostream>

int main() {
    try {
        if (optimization::method_from_string("L-SHADE") !=
                optimization::Method::lshade ||
            optimization::method_name(optimization::Method::sade) != "sade") {
            std::cerr << "Incorrect method name conversion\n";
            return 1;
        }

        const optimization::SolverSettings settings =
            optimization::load_solver_settings(TEST_OPTIMIZER_CONFIG_PATH);
        if (settings.method != optimization::Method::pso || settings.bounds.size() != 2) {
            std::cerr << "Incorrect unified YAML settings\n";
            return 1;
        }

        const optimization::Solver solver(settings);
        const optimization::Result result = solver.optimize(
            [](const optimization::Vector& x) {
                return x[0] * x[0] + x[1] * x[1];
            });
        if (result.best_value >= 1e-6) {
            std::cerr << "Unified solver did not converge: " << result.best_value << '\n';
            return 1;
        }

        const optimization::SolverSettings de_settings =
            optimization::load_solver_settings(TEST_DE_CONFIG_PATH);
        if (de_settings.method != optimization::Method::de ||
            de_settings.de.variant != de::Variant::de ||
            de_settings.de.common.crossover != de::Crossover::exponential ||
            de_settings.de.basic.mutation_strategy !=
                de::MutationStrategy::current_to_best1) {
            std::cerr << "Incorrect DE selectors from unified YAML\n";
            return 1;
        }
        const optimization::Result de_result = optimization::Solver(de_settings).optimize(
            [](const optimization::Vector& x) {
                return x[0] * x[0] + x[1] * x[1];
            });
        if (de_result.best_value >= 1e-6) {
            std::cerr << "Unified DE solver did not converge: "
                      << de_result.best_value << '\n';
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
