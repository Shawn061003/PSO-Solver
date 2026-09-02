#include "optimization/solver.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <utility>

namespace optimization {
namespace {

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

Bounds load_bounds(const pso::YamlConfig& yaml) {
    const auto lower = yaml.get_double_list("bounds.lower");
    const auto upper = yaml.get_double_list("bounds.upper");
    if (lower.empty() || lower.size() != upper.size()) {
        throw std::runtime_error(
            "bounds.lower and bounds.upper must have the same non-zero length");
    }

    Bounds bounds;
    bounds.reserve(lower.size());
    for (std::size_t index = 0; index < lower.size(); ++index) {
        if (lower[index] >= upper[index]) {
            throw std::runtime_error(
                "Each bounds.lower value must be smaller than bounds.upper");
        }
        bounds.emplace_back(lower[index], upper[index]);
    }
    return bounds;
}

de::Crossover crossover_from_string(const std::string& name) {
    const std::string normalized = lowercase(name);
    if (normalized == "binomial" || normalized == "bin") {
        return de::Crossover::binomial;
    }
    if (normalized == "exponential" || normalized == "exp") {
        return de::Crossover::exponential;
    }
    throw std::runtime_error(
        "de_common.crossover must be binomial or exponential");
}

de::MutationStrategy mutation_from_string(const std::string& name) {
    const std::string normalized = lowercase(name);
    if (normalized == "rand1" || normalized == "rand/1") {
        return de::MutationStrategy::rand1;
    }
    if (normalized == "best1" || normalized == "best/1") {
        return de::MutationStrategy::best1;
    }
    if (normalized == "current_to_best1" || normalized == "current-to-best/1") {
        return de::MutationStrategy::current_to_best1;
    }
    if (normalized == "rand2" || normalized == "rand/2") {
        return de::MutationStrategy::rand2;
    }
    if (normalized == "best2" || normalized == "best/2") {
        return de::MutationStrategy::best2;
    }
    throw std::runtime_error(
        "de.mutation_strategy must be rand1, best1, current_to_best1, rand2 or best2");
}

}  // namespace

Method method_from_string(std::string_view name) {
    const std::string normalized = lowercase(std::string(name));
    if (normalized == "pso") {
        return Method::pso;
    }
    if (normalized == "de") {
        return Method::de;
    }
    if (normalized == "sade") {
        return Method::sade;
    }
    if (normalized == "jade") {
        return Method::jade;
    }
    if (normalized == "shade") {
        return Method::shade;
    }
    if (normalized == "lshade" || normalized == "l-shade") {
        return Method::lshade;
    }
    throw std::runtime_error(
        "solver.method must be one of: pso, de, sade, jade, shade, lshade");
}

std::string_view method_name(Method method) {
    switch (method) {
    case Method::pso:
        return "pso";
    case Method::de:
        return "de";
    case Method::sade:
        return "sade";
    case Method::jade:
        return "jade";
    case Method::shade:
        return "shade";
    case Method::lshade:
        return "lshade";
    }
    throw std::logic_error("Unknown optimization method");
}

SolverSettings load_solver_settings(const std::filesystem::path& path) {
    const pso::YamlConfig yaml = pso::YamlConfig::load(path);
    SolverSettings settings;
    settings.method = method_from_string(yaml.get_string("solver.method"));
    settings.bounds = load_bounds(yaml);

    // 这些字段对所有算法含义相同，切换 method 时不需要重复修改。
    const bool maximize = yaml.get_bool("solver.maximize");
    const std::size_t max_iterations = yaml.get_size("solver.max_iterations");
    const double tolerance = yaml.get_double("solver.tolerance");
    const std::size_t stall_iterations = yaml.get_size("solver.stall_iterations");
    const std::uint64_t seed = yaml.get_uint64("solver.seed");

    settings.pso.maximize = maximize;
    settings.pso.max_iterations = max_iterations;
    settings.pso.tolerance = tolerance;
    settings.pso.stall_iterations = stall_iterations;
    settings.pso.seed = seed;

    settings.de.common.maximize = maximize;
    settings.de.common.max_generations = max_iterations;
    settings.de.common.tolerance = tolerance;
    settings.de.common.stall_generations = stall_iterations;
    settings.de.common.seed = seed;

    if (settings.method == Method::pso) {
        settings.pso.swarm_size = yaml.get_size("pso.swarm_size");
        settings.pso.inertia_start = yaml.get_double("pso.inertia_start");
        settings.pso.inertia_end = yaml.get_double("pso.inertia_end");
        settings.pso.cognitive = yaml.get_double("pso.cognitive");
        settings.pso.social = yaml.get_double("pso.social");
        settings.pso.velocity_limit_ratio = yaml.get_double("pso.velocity_limit_ratio");
        settings.pso.boundary_damping = yaml.get_double("pso.boundary_damping");
        return settings;
    }

    // DE 系列共同使用同一个交叉方式选择器。
    settings.de.common.crossover = crossover_from_string(
        yaml.get_string("de_common.crossover"));

    switch (settings.method) {
    case Method::de:
        settings.de.variant = de::Variant::de;
        settings.de.basic.population_size = yaml.get_size("de.population_size");
        settings.de.basic.differential_weight = yaml.get_double("de.differential_weight");
        settings.de.basic.crossover_rate = yaml.get_double("de.crossover_rate");
        settings.de.basic.mutation_strategy = mutation_from_string(
            yaml.get_string("de.mutation_strategy"));
        break;
    case Method::sade:
        settings.de.variant = de::Variant::sade;
        settings.de.sade.population_size = yaml.get_size("sade.population_size");
        settings.de.sade.learning_period = yaml.get_size("sade.learning_period");
        settings.de.sade.differential_weight_mean =
            yaml.get_double("sade.differential_weight_mean");
        settings.de.sade.differential_weight_stddev =
            yaml.get_double("sade.differential_weight_stddev");
        settings.de.sade.initial_crossover_mean =
            yaml.get_double("sade.initial_crossover_mean");
        settings.de.sade.crossover_stddev = yaml.get_double("sade.crossover_stddev");
        break;
    case Method::jade:
        settings.de.variant = de::Variant::jade;
        settings.de.jade.population_size = yaml.get_size("jade.population_size");
        settings.de.jade.p_best_rate = yaml.get_double("jade.p_best_rate");
        settings.de.jade.adaptation_rate = yaml.get_double("jade.adaptation_rate");
        settings.de.jade.initial_f_mean = yaml.get_double("jade.initial_f_mean");
        settings.de.jade.initial_cr_mean = yaml.get_double("jade.initial_cr_mean");
        break;
    case Method::shade:
        settings.de.variant = de::Variant::shade;
        settings.de.shade.population_size = yaml.get_size("shade.population_size");
        settings.de.shade.p_best_rate = yaml.get_double("shade.p_best_rate");
        settings.de.shade.memory_size = yaml.get_size("shade.memory_size");
        settings.de.shade.initial_f_memory = yaml.get_double("shade.initial_f_memory");
        settings.de.shade.initial_cr_memory = yaml.get_double("shade.initial_cr_memory");
        break;
    case Method::lshade:
        settings.de.variant = de::Variant::lshade;
        settings.de.lshade.initial_population_size =
            yaml.get_size("lshade.initial_population_size");
        settings.de.lshade.min_population_size =
            yaml.get_size("lshade.min_population_size");
        settings.de.lshade.p_best_rate = yaml.get_double("lshade.p_best_rate");
        settings.de.lshade.memory_size = yaml.get_size("lshade.memory_size");
        settings.de.lshade.initial_f_memory = yaml.get_double("lshade.initial_f_memory");
        settings.de.lshade.initial_cr_memory = yaml.get_double("lshade.initial_cr_memory");
        break;
    case Method::pso:
        break;
    }
    return settings;
}

Solver::Solver(SolverSettings settings)
    : settings_(std::move(settings)) {}

Result Solver::optimize(
    const Objective& objective,
    const std::vector<Vector>& initial_positions,
    const Callback& callback) const {
    if (settings_.method == Method::pso) {
        const pso::ParticleSwarmOptimizer optimizer(settings_.bounds, settings_.pso);
        return optimizer.optimize(objective, initial_positions, callback);
    }
    const de::DifferentialEvolutionOptimizer optimizer(settings_.bounds, settings_.de);
    return optimizer.optimize(objective, initial_positions, callback);
}

Method Solver::method() const noexcept {
    return settings_.method;
}

}  // namespace optimization
