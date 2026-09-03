#include "optimization/solver.hpp"

#include <exception>
#include <iostream>

int main() {
    try {
        if (optimization::method_from_string("L-SHADE") !=
                optimization::Method::lshade ||
            optimization::method_from_string("CMA-ES") !=
                optimization::Method::cmaes ||
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

        const optimization::SolverSettings es_settings =
            optimization::load_solver_settings(TEST_ES_CONFIG_PATH);
        if (es_settings.method != optimization::Method::es ||
            es_settings.es.selection_mode != es::SelectionMode::plus ||
            es_settings.es.recombination != es::Recombination::intermediate) {
            std::cerr << "Incorrect ES selectors from unified YAML\n";
            return 1;
        }
        const optimization::Result es_result = optimization::Solver(es_settings).optimize(
            [](const optimization::Vector& x) {
                const double first = x[0] - 1.0;
                const double second = x[1] + 2.0;
                return first * first + second * second;
            });
        if (es_result.best_value >= 1e-4) {
            std::cerr << "Unified ES solver did not converge: "
                      << es_result.best_value << '\n';
            return 1;
        }

        const optimization::SolverSettings cmaes_settings =
            optimization::load_solver_settings(TEST_CMAES_CONFIG_PATH);
        if (cmaes_settings.method != optimization::Method::cmaes ||
            cmaes_settings.cmaes.population_size != 0 ||
            cmaes_settings.cmaes.parent_count != 0) {
            std::cerr << "Incorrect CMA-ES settings from unified YAML\n";
            return 1;
        }
        const optimization::Result cmaes_result =
            optimization::Solver(cmaes_settings).optimize(
                [](const optimization::Vector& x) {
                    const double first = x[0] - 1.0;
                    const double second = x[1] + 2.0;
                    return first * first + second * second;
                });
        if (cmaes_result.best_value >= 1e-7) {
            std::cerr << "Unified CMA-ES solver did not converge: "
                      << cmaes_result.best_value << '\n';
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
