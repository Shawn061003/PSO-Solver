#include "de/de.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>
#include <utility>

namespace de {
namespace {

using optimization::Bounds;
using optimization::Callback;
using optimization::Objective;
using optimization::Result;
using optimization::Vector;

struct Individual {
    Vector position;
    double value{};
};

bool better(double left, double right, bool maximize) {
    return maximize ? left > right : left < right;
}

bool at_least_as_good(double trial, double target, bool maximize) {
    return better(trial, target, maximize) || trial == target;
}

void validate_unit_interval(double value, const char* name) {
    if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
        throw std::invalid_argument(std::string(name) + " must be in [0, 1]");
    }
}

void validate_positive(double value, const char* name) {
    if (!std::isfinite(value) || value <= 0.0) {
        throw std::invalid_argument(std::string(name) + " must be positive");
    }
}

void validate_config(const Bounds& bounds, const DeConfig& config) {
    if (bounds.empty()) {
        throw std::invalid_argument("DE needs at least one dimension");
    }
    for (const auto& [lower, upper] : bounds) {
        if (!std::isfinite(lower) || !std::isfinite(upper) || lower >= upper) {
            throw std::invalid_argument(
                "Each finite lower bound must be less than its upper bound");
        }
    }
    if (config.common.max_generations == 0 || config.common.stall_generations == 0) {
        throw std::invalid_argument("DE generation counts must be positive");
    }
    if (!std::isfinite(config.common.tolerance) || config.common.tolerance < 0.0) {
        throw std::invalid_argument("DE tolerance must be finite and non-negative");
    }

    switch (config.variant) {
    case Variant::de:
        if (config.basic.population_size < 4 ||
            ((config.basic.mutation_strategy == MutationStrategy::rand2 ||
              config.basic.mutation_strategy == MutationStrategy::best2) &&
             config.basic.population_size < 6)) {
            throw std::invalid_argument(
                "Basic DE population_size must be at least 4, or at least 6 for rand2/best2");
        }
        if (!std::isfinite(config.basic.differential_weight) ||
            config.basic.differential_weight <= 0.0 ||
            config.basic.differential_weight > 2.0) {
            throw std::invalid_argument("DE differential_weight must be in (0, 2]");
        }
        validate_unit_interval(config.basic.crossover_rate, "DE crossover_rate");
        break;
    case Variant::sade:
        if (config.sade.population_size < 6 || config.sade.learning_period == 0) {
            throw std::invalid_argument(
                "SaDE population_size must be at least 6 and learning_period must be positive");
        }
        validate_positive(
            config.sade.differential_weight_mean, "SaDE differential_weight_mean");
        validate_positive(
            config.sade.differential_weight_stddev, "SaDE differential_weight_stddev");
        validate_unit_interval(config.sade.initial_crossover_mean, "SaDE initial_crossover_mean");
        validate_positive(config.sade.crossover_stddev, "SaDE crossover_stddev");
        break;
    case Variant::jade:
        if (config.jade.population_size < 4) {
            throw std::invalid_argument("JADE population_size must be at least 4");
        }
        validate_positive(config.jade.p_best_rate, "JADE p_best_rate");
        if (config.jade.p_best_rate > 1.0) {
            throw std::invalid_argument("JADE p_best_rate must not exceed 1");
        }
        validate_positive(config.jade.adaptation_rate, "JADE adaptation_rate");
        if (config.jade.adaptation_rate > 1.0) {
            throw std::invalid_argument("JADE adaptation_rate must not exceed 1");
        }
        validate_unit_interval(config.jade.initial_f_mean, "JADE initial_f_mean");
        validate_unit_interval(config.jade.initial_cr_mean, "JADE initial_cr_mean");
        break;
    case Variant::shade:
        if (config.shade.population_size < 4 || config.shade.memory_size == 0) {
            throw std::invalid_argument(
                "SHADE population_size must be at least 4 and memory_size must be positive");
        }
        validate_positive(config.shade.p_best_rate, "SHADE p_best_rate");
        if (config.shade.p_best_rate > 1.0) {
            throw std::invalid_argument("SHADE p_best_rate must not exceed 1");
        }
        validate_unit_interval(config.shade.initial_f_memory, "SHADE initial_f_memory");
        validate_unit_interval(config.shade.initial_cr_memory, "SHADE initial_cr_memory");
        break;
    case Variant::lshade:
        if (config.lshade.min_population_size < 4 ||
            config.lshade.initial_population_size < config.lshade.min_population_size ||
            config.lshade.memory_size == 0) {
            throw std::invalid_argument(
                "L-SHADE needs initial_population_size >= min_population_size >= 4 and H > 0");
        }
        validate_positive(config.lshade.p_best_rate, "L-SHADE p_best_rate");
        if (config.lshade.p_best_rate > 1.0) {
            throw std::invalid_argument("L-SHADE p_best_rate must not exceed 1");
        }
        validate_unit_interval(config.lshade.initial_f_memory, "L-SHADE initial_f_memory");
        validate_unit_interval(config.lshade.initial_cr_memory, "L-SHADE initial_cr_memory");
        break;
    }
}

double evaluate(const Objective& objective, const Vector& position) {
    const double value = objective(position);
    if (std::isnan(value)) {
        throw std::runtime_error("Objective returned NaN");
    }
    return value;
}

std::vector<Individual> initialize_population(
    std::size_t population_size,
    const Bounds& bounds,
    const std::vector<Vector>& initial_positions,
    const Objective& objective,
    std::mt19937_64& random,
    std::size_t& evaluations) {
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::vector<Individual> population(population_size);
    for (std::size_t index = 0; index < population_size; ++index) {
        auto& position = population[index].position;
        position.resize(bounds.size());
        for (std::size_t dimension = 0; dimension < bounds.size(); ++dimension) {
            const auto [lower, upper] = bounds[dimension];
            position[dimension] = lower + unit(random) * (upper - lower);
        }

        // 热启动点按顺序覆盖前若干随机个体；轻微越界会裁剪到最近边界。
        if (index < initial_positions.size()) {
            if (initial_positions[index].size() != bounds.size()) {
                throw std::invalid_argument("Initial individual has the wrong dimension");
            }
            for (std::size_t dimension = 0; dimension < bounds.size(); ++dimension) {
                position[dimension] = std::clamp(
                    initial_positions[index][dimension],
                    bounds[dimension].first,
                    bounds[dimension].second);
            }
        }
        population[index].value = evaluate(objective, position);
        ++evaluations;
    }
    return population;
}

std::size_t best_index(const std::vector<Individual>& population, bool maximize) {
    std::size_t result = 0;
    for (std::size_t index = 1; index < population.size(); ++index) {
        if (better(population[index].value, population[result].value, maximize)) {
            result = index;
        }
    }
    return result;
}

std::vector<std::size_t> ranked_indices(
    const std::vector<Individual>& population,
    bool maximize) {
    std::vector<std::size_t> indices(population.size());
    std::iota(indices.begin(), indices.end(), 0);
    std::sort(indices.begin(), indices.end(), [&](std::size_t left, std::size_t right) {
        return better(population[left].value, population[right].value, maximize);
    });
    return indices;
}

std::vector<std::size_t> distinct_indices(
    std::size_t count,
    std::size_t population_size,
    std::size_t excluded,
    std::mt19937_64& random) {
    if (count >= population_size) {
        throw std::logic_error("Not enough distinct DE donor individuals");
    }
    std::uniform_int_distribution<std::size_t> choose(0, population_size - 1);
    std::vector<std::size_t> result;
    result.reserve(count);
    while (result.size() < count) {
        const std::size_t candidate = choose(random);
        if (candidate != excluded &&
            std::find(result.begin(), result.end(), candidate) == result.end()) {
            result.push_back(candidate);
        }
    }
    return result;
}

// 越界分量移到“原目标个体与最近边界”的中点。该方法不会让大量个体粘在边界上。
void repair_bounds(Vector& mutant, const Vector& target, const Bounds& bounds) {
    for (std::size_t dimension = 0; dimension < bounds.size(); ++dimension) {
        const auto [lower, upper] = bounds[dimension];
        if (mutant[dimension] < lower) {
            mutant[dimension] = 0.5 * (lower + target[dimension]);
        } else if (mutant[dimension] > upper) {
            mutant[dimension] = 0.5 * (upper + target[dimension]);
        }
        mutant[dimension] = std::clamp(mutant[dimension], lower, upper);
    }
}

Vector binomial_crossover(
    const Vector& target,
    const Vector& mutant,
    double crossover_rate,
    std::mt19937_64& random) {
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::uniform_int_distribution<std::size_t> forced(0, target.size() - 1);
    const std::size_t forced_dimension = forced(random);
    Vector trial(target.size());
    for (std::size_t dimension = 0; dimension < target.size(); ++dimension) {
        trial[dimension] =
            (dimension == forced_dimension || unit(random) <= crossover_rate)
            ? mutant[dimension]
            : target[dimension];
    }
    return trial;
}

Vector exponential_crossover(
    const Vector& target,
    const Vector& mutant,
    double crossover_rate,
    std::mt19937_64& random) {
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::uniform_int_distribution<std::size_t> choose_start(0, target.size() - 1);
    const std::size_t start = choose_start(random);
    Vector trial = target;

    // 指数交叉从 start 开始连续复制变异分量，并按维数循环；length 至少为 1。
    std::size_t length = 0;
    do {
        const std::size_t dimension = (start + length) % target.size();
        trial[dimension] = mutant[dimension];
        ++length;
    } while (length < target.size() && unit(random) <= crossover_rate);
    return trial;
}

Vector crossover(
    const Vector& target,
    const Vector& mutant,
    double crossover_rate,
    Crossover type,
    std::mt19937_64& random) {
    return type == Crossover::binomial
        ? binomial_crossover(target, mutant, crossover_rate, random)
        : exponential_crossover(target, mutant, crossover_rate, random);
}

double sample_positive_cauchy(double location, std::mt19937_64& random) {
    std::cauchy_distribution<double> distribution(location, 0.1);
    for (;;) {
        const double sample = distribution(random);
        if (sample > 0.0) {
            return std::min(sample, 1.0);
        }
    }
}

double sample_clamped_normal(double mean, double stddev, std::mt19937_64& random) {
    std::normal_distribution<double> distribution(mean, stddev);
    return std::clamp(distribution(random), 0.0, 1.0);
}

std::size_t choose_pbest(
    const std::vector<Individual>& population,
    double p_best_rate,
    bool maximize,
    std::mt19937_64& random) {
    const auto ranked = ranked_indices(population, maximize);
    const std::size_t top_count = std::min(
        population.size(),
        std::max<std::size_t>(2, static_cast<std::size_t>(
            std::ceil(p_best_rate * static_cast<double>(population.size())))));
    std::uniform_int_distribution<std::size_t> choose(0, top_count - 1);
    return ranked[choose(random)];
}

Vector choose_population_or_archive(
    const std::vector<Individual>& population,
    const std::vector<Vector>& archive,
    std::size_t target_index,
    std::size_t first_donor,
    std::mt19937_64& random) {
    const std::size_t total = population.size() + archive.size();
    std::uniform_int_distribution<std::size_t> choose(0, total - 1);
    for (;;) {
        const std::size_t candidate = choose(random);
        if (candidate < population.size()) {
            if (candidate == target_index || candidate == first_donor) {
                continue;
            }
            return population[candidate].position;
        }
        return archive[candidate - population.size()];
    }
}

void limit_archive(
    std::vector<Vector>& archive,
    std::size_t maximum_size,
    std::mt19937_64& random) {
    while (archive.size() > maximum_size) {
        std::uniform_int_distribution<std::size_t> choose(0, archive.size() - 1);
        const std::size_t index = choose(random);
        archive[index] = std::move(archive.back());
        archive.pop_back();
    }
}

bool update_result_after_generation(
    const std::vector<Individual>& population,
    const CommonConfig& common,
    std::size_t generation,
    std::size_t& stall_count,
    Result& result,
    const Callback& callback) {
    const Individual& candidate = population[best_index(population, common.maximize)];
    if (better(candidate.value, result.best_value, common.maximize)) {
        const double improvement = std::abs(candidate.value - result.best_value);
        result.best_value = candidate.value;
        result.best_position = candidate.position;
        stall_count = improvement > common.tolerance ? 0 : stall_count + 1;
    } else {
        ++stall_count;
    }

    result.iterations = generation;
    result.history.push_back(result.best_value);
    if (callback) {
        callback(generation, result.best_value, result.best_position);
    }
    if (stall_count >= common.stall_generations) {
        result.converged = true;
        return true;
    }
    return false;
}

Result make_initial_result(
    const std::vector<Individual>& population,
    std::size_t evaluations,
    bool maximize) {
    const Individual& best = population[best_index(population, maximize)];
    Result result;
    result.best_position = best.position;
    result.best_value = best.value;
    result.evaluations = evaluations;
    result.history.push_back(best.value);
    return result;
}

Result run_basic(
    const Bounds& bounds,
    const DeConfig& config,
    const Objective& objective,
    const std::vector<Vector>& initial_positions,
    const Callback& callback) {
    std::mt19937_64 random(config.common.seed);
    std::size_t evaluations = 0;
    auto population = initialize_population(
        config.basic.population_size, bounds, initial_positions, objective, random, evaluations);
    Result result = make_initial_result(population, evaluations, config.common.maximize);
    std::size_t stall_count = 0;

    for (std::size_t generation = 1;
         generation <= config.common.max_generations;
         ++generation) {
        const Individual generation_best =
            population[best_index(population, config.common.maximize)];
        auto next = population;
        for (std::size_t target = 0; target < population.size(); ++target) {
            Vector mutant(bounds.size());
            const double f = config.basic.differential_weight;
            switch (config.basic.mutation_strategy) {
            case MutationStrategy::rand1: {
                const auto r = distinct_indices(3, population.size(), target, random);
                for (std::size_t d = 0; d < bounds.size(); ++d) {
                    mutant[d] = population[r[0]].position[d] +
                        f * (population[r[1]].position[d] - population[r[2]].position[d]);
                }
                break;
            }
            case MutationStrategy::best1: {
                const auto r = distinct_indices(2, population.size(), target, random);
                for (std::size_t d = 0; d < bounds.size(); ++d) {
                    mutant[d] = generation_best.position[d] +
                        f * (population[r[0]].position[d] - population[r[1]].position[d]);
                }
                break;
            }
            case MutationStrategy::current_to_best1: {
                const auto r = distinct_indices(2, population.size(), target, random);
                for (std::size_t d = 0; d < bounds.size(); ++d) {
                    mutant[d] = population[target].position[d] +
                        f * (generation_best.position[d] - population[target].position[d]) +
                        f * (population[r[0]].position[d] - population[r[1]].position[d]);
                }
                break;
            }
            case MutationStrategy::rand2: {
                const auto r = distinct_indices(5, population.size(), target, random);
                for (std::size_t d = 0; d < bounds.size(); ++d) {
                    mutant[d] = population[r[0]].position[d] +
                        f * (population[r[1]].position[d] - population[r[2]].position[d]) +
                        f * (population[r[3]].position[d] - population[r[4]].position[d]);
                }
                break;
            }
            case MutationStrategy::best2: {
                const auto r = distinct_indices(4, population.size(), target, random);
                for (std::size_t d = 0; d < bounds.size(); ++d) {
                    mutant[d] = generation_best.position[d] +
                        f * (population[r[0]].position[d] - population[r[1]].position[d]) +
                        f * (population[r[2]].position[d] - population[r[3]].position[d]);
                }
                break;
            }
            }
            repair_bounds(mutant, population[target].position, bounds);
            Vector trial = crossover(
                population[target].position,
                mutant,
                config.basic.crossover_rate,
                config.common.crossover,
                random);
            const double trial_value = evaluate(objective, trial);
            ++result.evaluations;
            if (at_least_as_good(
                    trial_value, population[target].value, config.common.maximize)) {
                next[target] = {std::move(trial), trial_value};
            }
        }
        population = std::move(next);
        if (update_result_after_generation(
                population, config.common, generation, stall_count, result, callback)) {
            break;
        }
    }
    return result;
}

Result run_sade(
    const Bounds& bounds,
    const DeConfig& config,
    const Objective& objective,
    const std::vector<Vector>& initial_positions,
    const Callback& callback) {
    constexpr std::size_t strategy_count = 4;
    std::mt19937_64 random(config.common.seed);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::size_t evaluations = 0;
    auto population = initialize_population(
        config.sade.population_size, bounds, initial_positions, objective, random, evaluations);
    Result result = make_initial_result(population, evaluations, config.common.maximize);

    std::array<double, strategy_count> probabilities{0.25, 0.25, 0.25, 0.25};
    std::array<double, strategy_count> crossover_means{};
    crossover_means.fill(config.sade.initial_crossover_mean);
    std::array<std::size_t, strategy_count> successes{};
    std::array<std::size_t, strategy_count> failures{};
    std::array<std::vector<double>, strategy_count> successful_crossover;
    std::size_t stall_count = 0;

    for (std::size_t generation = 1;
         generation <= config.common.max_generations;
         ++generation) {
        const Individual generation_best =
            population[best_index(population, config.common.maximize)];
        auto next = population;
        std::discrete_distribution<std::size_t> choose_strategy(
            probabilities.begin(), probabilities.end());

        for (std::size_t target = 0; target < population.size(); ++target) {
            const std::size_t strategy = choose_strategy(random);
            std::normal_distribution<double> f_distribution(
                config.sade.differential_weight_mean,
                config.sade.differential_weight_stddev);
            double differential_weight = 0.0;
            do {
                differential_weight = f_distribution(random);
            } while (differential_weight <= 0.0);
            differential_weight = std::min(differential_weight, 1.0);
            const double crossover_rate = sample_clamped_normal(
                crossover_means[strategy], config.sade.crossover_stddev, random);

            Vector mutant(bounds.size());
            if (strategy == 0) {
                // Strategy 0: DE/rand/1/bin。
                const auto r = distinct_indices(3, population.size(), target, random);
                for (std::size_t d = 0; d < bounds.size(); ++d) {
                    mutant[d] = population[r[0]].position[d] + differential_weight *
                        (population[r[1]].position[d] - population[r[2]].position[d]);
                }
            } else if (strategy == 1) {
                // Strategy 1: DE/rand-to-best/2/bin。
                const auto r = distinct_indices(5, population.size(), target, random);
                for (std::size_t d = 0; d < bounds.size(); ++d) {
                    mutant[d] = population[r[0]].position[d] + differential_weight *
                        (generation_best.position[d] - population[r[0]].position[d]) +
                        differential_weight *
                        (population[r[1]].position[d] - population[r[2]].position[d]) +
                        differential_weight *
                        (population[r[3]].position[d] - population[r[4]].position[d]);
                }
            } else if (strategy == 2) {
                // Strategy 2: DE/rand/2/bin。
                const auto r = distinct_indices(5, population.size(), target, random);
                for (std::size_t d = 0; d < bounds.size(); ++d) {
                    mutant[d] = population[r[0]].position[d] + differential_weight *
                        (population[r[1]].position[d] - population[r[2]].position[d]) +
                        differential_weight *
                        (population[r[3]].position[d] - population[r[4]].position[d]);
                }
            } else {
                // Strategy 3: DE/current-to-rand/1；变异后仍使用用户选择的统一交叉方式。
                const auto r = distinct_indices(3, population.size(), target, random);
                const double attraction = unit(random);
                for (std::size_t d = 0; d < bounds.size(); ++d) {
                    mutant[d] = population[target].position[d] + attraction *
                        (population[r[0]].position[d] - population[target].position[d]) +
                        differential_weight *
                        (population[r[1]].position[d] - population[r[2]].position[d]);
                }
            }

            repair_bounds(mutant, population[target].position, bounds);
            Vector trial = crossover(
                population[target].position,
                mutant,
                crossover_rate,
                config.common.crossover,
                random);
            const double trial_value = evaluate(objective, trial);
            ++result.evaluations;
            if (at_least_as_good(
                    trial_value, population[target].value, config.common.maximize)) {
                if (better(trial_value, population[target].value, config.common.maximize)) {
                    ++successes[strategy];
                    successful_crossover[strategy].push_back(crossover_rate);
                } else {
                    ++failures[strategy];
                }
                next[target] = {std::move(trial), trial_value};
            } else {
                ++failures[strategy];
            }
        }
        population = std::move(next);

        // 一个学习周期结束后，根据成功率更新策略概率，并用成功 CR 的均值更新采样中心。
        if (generation % config.sade.learning_period == 0) {
            double rate_sum = 0.0;
            for (std::size_t strategy = 0; strategy < strategy_count; ++strategy) {
                probabilities[strategy] =
                    (static_cast<double>(successes[strategy]) + 0.01) /
                    (static_cast<double>(successes[strategy] + failures[strategy]) + 0.02);
                rate_sum += probabilities[strategy];
                if (!successful_crossover[strategy].empty()) {
                    crossover_means[strategy] = std::accumulate(
                        successful_crossover[strategy].begin(),
                        successful_crossover[strategy].end(),
                        0.0) / static_cast<double>(successful_crossover[strategy].size());
                }
                successes[strategy] = 0;
                failures[strategy] = 0;
                successful_crossover[strategy].clear();
            }
            for (double& probability : probabilities) {
                probability /= rate_sum;
            }
        }

        if (update_result_after_generation(
                population, config.common, generation, stall_count, result, callback)) {
            break;
        }
    }
    return result;
}

Result run_jade(
    const Bounds& bounds,
    const DeConfig& config,
    const Objective& objective,
    const std::vector<Vector>& initial_positions,
    const Callback& callback) {
    std::mt19937_64 random(config.common.seed);
    std::size_t evaluations = 0;
    auto population = initialize_population(
        config.jade.population_size, bounds, initial_positions, objective, random, evaluations);
    Result result = make_initial_result(population, evaluations, config.common.maximize);
    std::vector<Vector> archive;
    double f_mean = config.jade.initial_f_mean;
    double cr_mean = config.jade.initial_cr_mean;
    std::size_t stall_count = 0;

    for (std::size_t generation = 1;
         generation <= config.common.max_generations;
         ++generation) {
        auto next = population;
        std::vector<double> successful_f;
        std::vector<double> successful_cr;
        for (std::size_t target = 0; target < population.size(); ++target) {
            const double f = sample_positive_cauchy(f_mean, random);
            const double cr = sample_clamped_normal(cr_mean, 0.1, random);
            const std::size_t pbest = choose_pbest(
                population, config.jade.p_best_rate, config.common.maximize, random);
            const std::size_t r1 = distinct_indices(1, population.size(), target, random)[0];
            const Vector r2 = choose_population_or_archive(
                population, archive, target, r1, random);

            Vector mutant(bounds.size());
            for (std::size_t d = 0; d < bounds.size(); ++d) {
                mutant[d] = population[target].position[d] +
                    f * (population[pbest].position[d] - population[target].position[d]) +
                    f * (population[r1].position[d] - r2[d]);
            }
            repair_bounds(mutant, population[target].position, bounds);
            Vector trial = crossover(
                population[target].position,
                mutant,
                cr,
                config.common.crossover,
                random);
            const double trial_value = evaluate(objective, trial);
            ++result.evaluations;
            if (at_least_as_good(
                    trial_value, population[target].value, config.common.maximize)) {
                if (better(trial_value, population[target].value, config.common.maximize)) {
                    archive.push_back(population[target].position);
                    successful_f.push_back(f);
                    successful_cr.push_back(cr);
                }
                next[target] = {std::move(trial), trial_value};
            }
        }
        population = std::move(next);
        limit_archive(archive, population.size(), random);

        if (!successful_f.empty()) {
            const double f_sum = std::accumulate(successful_f.begin(), successful_f.end(), 0.0);
            const double f_square_sum = std::inner_product(
                successful_f.begin(), successful_f.end(), successful_f.begin(), 0.0);
            const double cr_average = std::accumulate(
                successful_cr.begin(), successful_cr.end(), 0.0) /
                static_cast<double>(successful_cr.size());
            const double c = config.jade.adaptation_rate;
            f_mean = (1.0 - c) * f_mean + c * (f_square_sum / f_sum);
            cr_mean = (1.0 - c) * cr_mean + c * cr_average;
        }

        if (update_result_after_generation(
                population, config.common, generation, stall_count, result, callback)) {
            break;
        }
    }
    return result;
}

void update_shade_memory(
    const std::vector<double>& successful_f,
    const std::vector<double>& successful_cr,
    const std::vector<double>& improvements,
    std::vector<double>& f_memory,
    std::vector<double>& cr_memory,
    std::size_t& memory_index) {
    if (successful_f.empty()) {
        return;
    }
    const double total_improvement =
        std::accumulate(improvements.begin(), improvements.end(), 0.0);
    double weighted_f_numerator = 0.0;
    double weighted_f_denominator = 0.0;
    double weighted_cr = 0.0;
    for (std::size_t index = 0; index < successful_f.size(); ++index) {
        const double weight = total_improvement > 0.0
            ? improvements[index] / total_improvement
            : 1.0 / static_cast<double>(successful_f.size());
        weighted_f_numerator += weight * successful_f[index] * successful_f[index];
        weighted_f_denominator += weight * successful_f[index];
        weighted_cr += weight * successful_cr[index];
    }
    if (weighted_f_denominator > 0.0) {
        f_memory[memory_index] = weighted_f_numerator / weighted_f_denominator;
    }
    cr_memory[memory_index] = weighted_cr;
    memory_index = (memory_index + 1) % f_memory.size();
}

Result run_shade_family(
    const Bounds& bounds,
    const DeConfig& config,
    const Objective& objective,
    const std::vector<Vector>& initial_positions,
    const Callback& callback,
    bool linear_population_reduction) {
    const std::size_t initial_size = linear_population_reduction
        ? config.lshade.initial_population_size
        : config.shade.population_size;
    const std::size_t minimum_size = linear_population_reduction
        ? config.lshade.min_population_size
        : initial_size;
    const double p_best_rate = linear_population_reduction
        ? config.lshade.p_best_rate
        : config.shade.p_best_rate;
    const std::size_t memory_size = linear_population_reduction
        ? config.lshade.memory_size
        : config.shade.memory_size;
    const double initial_f = linear_population_reduction
        ? config.lshade.initial_f_memory
        : config.shade.initial_f_memory;
    const double initial_cr = linear_population_reduction
        ? config.lshade.initial_cr_memory
        : config.shade.initial_cr_memory;

    std::mt19937_64 random(config.common.seed);
    std::uniform_int_distribution<std::size_t> choose_memory(0, memory_size - 1);
    std::size_t evaluations = 0;
    auto population = initialize_population(
        initial_size, bounds, initial_positions, objective, random, evaluations);
    Result result = make_initial_result(population, evaluations, config.common.maximize);
    std::vector<Vector> archive;
    std::vector<double> f_memory(memory_size, initial_f);
    std::vector<double> cr_memory(memory_size, initial_cr);
    std::size_t memory_index = 0;
    std::size_t stall_count = 0;

    for (std::size_t generation = 1;
         generation <= config.common.max_generations;
         ++generation) {
        auto next = population;
        std::vector<double> successful_f;
        std::vector<double> successful_cr;
        std::vector<double> improvements;
        for (std::size_t target = 0; target < population.size(); ++target) {
            const std::size_t selected_memory = choose_memory(random);
            const double f = sample_positive_cauchy(f_memory[selected_memory], random);
            const double cr = sample_clamped_normal(cr_memory[selected_memory], 0.1, random);
            const std::size_t pbest = choose_pbest(
                population, p_best_rate, config.common.maximize, random);
            const std::size_t r1 = distinct_indices(1, population.size(), target, random)[0];
            const Vector r2 = choose_population_or_archive(
                population, archive, target, r1, random);

            Vector mutant(bounds.size());
            for (std::size_t d = 0; d < bounds.size(); ++d) {
                mutant[d] = population[target].position[d] +
                    f * (population[pbest].position[d] - population[target].position[d]) +
                    f * (population[r1].position[d] - r2[d]);
            }
            repair_bounds(mutant, population[target].position, bounds);
            Vector trial = crossover(
                population[target].position,
                mutant,
                cr,
                config.common.crossover,
                random);
            const double trial_value = evaluate(objective, trial);
            ++result.evaluations;
            if (at_least_as_good(
                    trial_value, population[target].value, config.common.maximize)) {
                if (better(trial_value, population[target].value, config.common.maximize)) {
                    archive.push_back(population[target].position);
                    successful_f.push_back(f);
                    successful_cr.push_back(cr);
                    improvements.push_back(std::abs(trial_value - population[target].value));
                }
                next[target] = {std::move(trial), trial_value};
            }
        }
        population = std::move(next);
        update_shade_memory(
            successful_f,
            successful_cr,
            improvements,
            f_memory,
            cr_memory,
            memory_index);

        if (linear_population_reduction) {
            // L-SHADE：按当前代数占最大代数的比例，将 NP 从初始值线性降到 NPmin。
            const double fraction = static_cast<double>(generation) /
                                    static_cast<double>(config.common.max_generations);
            const std::size_t target_size = std::max(
                minimum_size,
                static_cast<std::size_t>(std::llround(
                    static_cast<double>(initial_size) +
                    fraction * (static_cast<double>(minimum_size) -
                                static_cast<double>(initial_size)))));
            if (population.size() > target_size) {
                const auto ranked = ranked_indices(population, config.common.maximize);
                std::vector<Individual> reduced;
                reduced.reserve(target_size);
                for (std::size_t index = 0; index < target_size; ++index) {
                    reduced.push_back(std::move(population[ranked[index]]));
                }
                population = std::move(reduced);
            }
        }
        limit_archive(archive, population.size(), random);

        if (update_result_after_generation(
                population, config.common, generation, stall_count, result, callback)) {
            break;
        }
    }
    return result;
}

}  // namespace

DifferentialEvolutionOptimizer::DifferentialEvolutionOptimizer(
    optimization::Bounds bounds,
    DeConfig config)
    : bounds_(std::move(bounds)), config_(config) {
    validate_config(bounds_, config_);
}

optimization::Result DifferentialEvolutionOptimizer::optimize(
    const optimization::Objective& objective,
    const std::vector<optimization::Vector>& initial_positions,
    const optimization::Callback& callback) const {
    if (!objective) {
        throw std::invalid_argument("Objective function must not be empty");
    }
    const auto started = std::chrono::steady_clock::now();
    Result result;
    switch (config_.variant) {
    case Variant::de:
        result = run_basic(bounds_, config_, objective, initial_positions, callback);
        break;
    case Variant::sade:
        result = run_sade(bounds_, config_, objective, initial_positions, callback);
        break;
    case Variant::jade:
        result = run_jade(bounds_, config_, objective, initial_positions, callback);
        break;
    case Variant::shade:
        result = run_shade_family(
            bounds_, config_, objective, initial_positions, callback, false);
        break;
    case Variant::lshade:
        result = run_shade_family(
            bounds_, config_, objective, initial_positions, callback, true);
        break;
    }
    result.elapsed_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    return result;
}

}  // namespace de
