#include "es/es.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>

namespace es {
namespace {

using optimization::Bounds;
using optimization::Callback;
using optimization::Objective;
using optimization::Result;
using optimization::Vector;
using Matrix = std::vector<Vector>;

struct Candidate {
    // position 使用归一化坐标，每一维都位于 [0,1]。
    Vector position;
    double value{};
};

struct CmaCandidate {
    Vector position;
    Vector normalized_step;
    double value{};
};

bool better(double left, double right, bool maximize) {
    return maximize ? left > right : left < right;
}

void validate_bounds_and_common(const Bounds& bounds, const CommonConfig& common) {
    if (bounds.empty()) {
        throw std::invalid_argument("ES needs at least one dimension");
    }
    for (const auto& [lower, upper] : bounds) {
        if (!std::isfinite(lower) || !std::isfinite(upper) || lower >= upper) {
            throw std::invalid_argument(
                "Each finite lower bound must be less than its upper bound");
        }
    }
    if (common.max_generations == 0 || common.stall_generations == 0) {
        throw std::invalid_argument("ES generation counts must be positive");
    }
    if (!std::isfinite(common.tolerance) || common.tolerance < 0.0) {
        throw std::invalid_argument("ES tolerance must be finite and non-negative");
    }
}

void validate_es_config(const Bounds& bounds, const EsConfig& config) {
    validate_bounds_and_common(bounds, config.common);
    if (config.parent_count < 2 || config.offspring_count == 0) {
        throw std::invalid_argument(
            "ES parent_count must be at least 2 and offspring_count must be positive");
    }
    if (config.selection_mode == SelectionMode::comma &&
        config.offspring_count < config.parent_count) {
        throw std::invalid_argument(
            "Comma ES requires offspring_count >= parent_count");
    }
    if (!std::isfinite(config.initial_step_size) ||
        !std::isfinite(config.min_step_size) ||
        !std::isfinite(config.max_step_size) ||
        config.min_step_size <= 0.0 ||
        config.initial_step_size < config.min_step_size ||
        config.initial_step_size > config.max_step_size) {
        throw std::invalid_argument(
            "ES needs 0 < min_step_size <= initial_step_size <= max_step_size");
    }
    if (config.adapt_step_size &&
        (config.adaptation_interval == 0 ||
         !std::isfinite(config.adaptation_factor) ||
         config.adaptation_factor <= 1.0)) {
        throw std::invalid_argument(
            "ES step adaptation needs a positive interval and adaptation_factor > 1");
    }
}

struct ResolvedCmaParameters {
    std::size_t lambda{};
    std::size_t mu{};
    Vector weights;
    double mu_effective{};
    double c_sigma{};
    double d_sigma{};
    double c_c{};
    double c1{};
    double c_mu{};
    double expected_normal_length{};
};

double automatic_or_configured(double configured, double automatic, const char* name) {
    if (!std::isfinite(configured) || configured < 0.0) {
        throw std::invalid_argument(std::string(name) + " must be zero or positive");
    }
    return configured == 0.0 ? automatic : configured;
}

ResolvedCmaParameters resolve_cma_parameters(
    std::size_t dimensions,
    const CmaEsConfig& config) {
    ResolvedCmaParameters result;
    result.lambda = config.population_size == 0
        ? 4 + static_cast<std::size_t>(
            std::floor(3.0 * std::log(static_cast<double>(dimensions))))
        : config.population_size;
    result.lambda = std::max<std::size_t>(result.lambda, 2);
    result.mu = config.parent_count == 0 ? result.lambda / 2 : config.parent_count;
    if (result.mu == 0 || result.mu > result.lambda) {
        throw std::invalid_argument("CMA-ES needs 1 <= parent_count <= population_size");
    }

    result.weights.resize(result.mu);
    for (std::size_t index = 0; index < result.mu; ++index) {
        result.weights[index] =
            std::log(static_cast<double>(result.mu) + 0.5) -
            std::log(static_cast<double>(index) + 1.0);
    }
    const double weight_sum = std::accumulate(
        result.weights.begin(), result.weights.end(), 0.0);
    for (double& weight : result.weights) {
        weight /= weight_sum;
    }
    const double square_sum = std::inner_product(
        result.weights.begin(), result.weights.end(), result.weights.begin(), 0.0);
    result.mu_effective = 1.0 / square_sum;

    const double n = static_cast<double>(dimensions);
    const double mu_effective = result.mu_effective;
    const double automatic_c_sigma = (mu_effective + 2.0) /
                                     (n + mu_effective + 5.0);
    const double automatic_d_sigma = 1.0 +
        2.0 * std::max(0.0, std::sqrt((mu_effective - 1.0) / (n + 1.0)) - 1.0) +
        automatic_c_sigma;
    const double automatic_c_c = (4.0 + mu_effective / n) /
        (n + 4.0 + 2.0 * mu_effective / n);
    const double automatic_c1 = 2.0 /
        ((n + 1.3) * (n + 1.3) + mu_effective);
    const double automatic_c_mu = std::min(
        1.0 - automatic_c1,
        2.0 * (mu_effective - 2.0 + 1.0 / mu_effective) /
            ((n + 2.0) * (n + 2.0) + mu_effective));

    result.c_sigma = automatic_or_configured(
        config.c_sigma, automatic_c_sigma, "CMA-ES c_sigma");
    result.d_sigma = automatic_or_configured(
        config.d_sigma, automatic_d_sigma, "CMA-ES d_sigma");
    result.c_c = automatic_or_configured(config.c_c, automatic_c_c, "CMA-ES c_c");
    result.c1 = automatic_or_configured(config.c1, automatic_c1, "CMA-ES c1");
    result.c_mu = automatic_or_configured(config.c_mu, automatic_c_mu, "CMA-ES c_mu");

    if (result.c_sigma <= 0.0 || result.c_sigma > 1.0 ||
        result.c_c <= 0.0 || result.c_c > 1.0 ||
        result.d_sigma <= 0.0 || result.c1 < 0.0 || result.c_mu < 0.0 ||
        result.c1 + result.c_mu > 1.0) {
        throw std::invalid_argument(
            "Invalid CMA-ES learning rates: require c_sigma/c_c in (0,1], "
            "d_sigma > 0 and c1 + c_mu <= 1");
    }

    result.expected_normal_length = std::sqrt(n) *
        (1.0 - 1.0 / (4.0 * n) + 1.0 / (21.0 * n * n));
    return result;
}

ResolvedCmaParameters validate_cma_config(
    const Bounds& bounds,
    const CmaEsConfig& config) {
    validate_bounds_and_common(bounds, config.common);
    if (!std::isfinite(config.initial_step_size) ||
        !std::isfinite(config.min_step_size) ||
        !std::isfinite(config.max_step_size) ||
        config.min_step_size <= 0.0 ||
        config.initial_step_size < config.min_step_size ||
        config.initial_step_size > config.max_step_size) {
        throw std::invalid_argument(
            "CMA-ES needs 0 < min_step_size <= initial_step_size <= max_step_size");
    }
    if (config.eigen_update_period == 0 ||
        !std::isfinite(config.eigenvalue_floor) || config.eigenvalue_floor <= 0.0 ||
        !std::isfinite(config.max_condition_number) ||
        config.max_condition_number <= 1.0) {
        throw std::invalid_argument(
            "CMA-ES eigen settings need period > 0, floor > 0 and condition number > 1");
    }
    return resolve_cma_parameters(bounds.size(), config);
}

double reflect_unit(double value) {
    // 将任意实数折回 [0,1]，比简单截断更能保留高斯采样的变化方向。
    value = std::fmod(value, 2.0);
    if (value < 0.0) {
        value += 2.0;
    }
    return value <= 1.0 ? value : 2.0 - value;
}

Vector normalize_position(const Vector& position, const Bounds& bounds) {
    if (position.size() != bounds.size()) {
        throw std::invalid_argument("Initial ES position has the wrong dimension");
    }
    Vector normalized(bounds.size());
    for (std::size_t dimension = 0; dimension < bounds.size(); ++dimension) {
        normalized[dimension] = std::clamp(
            (position[dimension] - bounds[dimension].first) /
                (bounds[dimension].second - bounds[dimension].first),
            0.0,
            1.0);
    }
    return normalized;
}

Vector denormalize_position(const Vector& normalized, const Bounds& bounds) {
    Vector position(bounds.size());
    for (std::size_t dimension = 0; dimension < bounds.size(); ++dimension) {
        position[dimension] = bounds[dimension].first + normalized[dimension] *
            (bounds[dimension].second - bounds[dimension].first);
    }
    return position;
}

double evaluate(
    const Objective& objective,
    const Vector& normalized,
    const Bounds& bounds) {
    const double value = objective(denormalize_position(normalized, bounds));
    if (std::isnan(value)) {
        throw std::runtime_error("Objective returned NaN");
    }
    return value;
}

void sort_candidates(std::vector<Candidate>& candidates, bool maximize) {
    std::sort(candidates.begin(), candidates.end(), [&](const Candidate& left, const Candidate& right) {
        return better(left.value, right.value, maximize);
    });
}

void update_result(
    const Candidate& candidate,
    const CommonConfig& common,
    std::size_t generation,
    std::size_t& stall_count,
    Result& result,
    const Bounds& bounds,
    const Callback& callback) {
    if (better(candidate.value, result.best_value, common.maximize)) {
        const double improvement = std::abs(candidate.value - result.best_value);
        result.best_value = candidate.value;
        result.best_position = denormalize_position(candidate.position, bounds);
        stall_count = improvement > common.tolerance ? 0 : stall_count + 1;
    } else {
        ++stall_count;
    }
    result.iterations = generation;
    result.history.push_back(result.best_value);
    if (callback) {
        callback(generation, result.best_value, result.best_position);
    }
}

Matrix identity_matrix(std::size_t dimensions) {
    Matrix result(dimensions, Vector(dimensions, 0.0));
    for (std::size_t index = 0; index < dimensions; ++index) {
        result[index][index] = 1.0;
    }
    return result;
}

Vector matrix_vector_product(const Matrix& matrix, const Vector& vector) {
    Vector result(matrix.size(), 0.0);
    for (std::size_t row = 0; row < matrix.size(); ++row) {
        for (std::size_t column = 0; column < vector.size(); ++column) {
            result[row] += matrix[row][column] * vector[column];
        }
    }
    return result;
}

double vector_norm(const Vector& vector) {
    return std::sqrt(std::inner_product(vector.begin(), vector.end(), vector.begin(), 0.0));
}

// 对实对称矩阵使用 Jacobi 旋转求特征值和正交特征向量。
// ES 库保持零第三方依赖，因此没有引入 Eigen；该实现适合数模常见的中低维问题。
void symmetric_eigendecomposition(
    const Matrix& input,
    double eigenvalue_floor,
    Matrix& eigenvectors,
    Vector& square_roots,
    Matrix& inverse_square_root,
    double& condition_number) {
    const std::size_t dimensions = input.size();
    Matrix matrix = input;
    eigenvectors = identity_matrix(dimensions);

    const std::size_t maximum_rotations = 100 * dimensions * dimensions;
    for (std::size_t rotation = 0; rotation < maximum_rotations; ++rotation) {
        std::size_t p = 0;
        std::size_t q = dimensions > 1 ? 1 : 0;
        double largest = 0.0;
        for (std::size_t row = 0; row < dimensions; ++row) {
            for (std::size_t column = row + 1; column < dimensions; ++column) {
                const double magnitude = std::abs(matrix[row][column]);
                if (magnitude > largest) {
                    largest = magnitude;
                    p = row;
                    q = column;
                }
            }
        }
        double diagonal_scale = 1.0;
        for (std::size_t index = 0; index < dimensions; ++index) {
            diagonal_scale = std::max(diagonal_scale, std::abs(matrix[index][index]));
        }
        if (dimensions == 1 || largest <= 1e-14 * diagonal_scale) {
            break;
        }

        const double angle = 0.5 * std::atan2(
            2.0 * matrix[p][q], matrix[q][q] - matrix[p][p]);
        const double cosine = std::cos(angle);
        const double sine = std::sin(angle);
        const double app = matrix[p][p];
        const double aqq = matrix[q][q];
        const double apq = matrix[p][q];

        for (std::size_t index = 0; index < dimensions; ++index) {
            if (index == p || index == q) {
                continue;
            }
            const double aip = matrix[index][p];
            const double aiq = matrix[index][q];
            matrix[index][p] = matrix[p][index] = cosine * aip - sine * aiq;
            matrix[index][q] = matrix[q][index] = sine * aip + cosine * aiq;
        }
        matrix[p][p] = cosine * cosine * app - 2.0 * sine * cosine * apq +
                       sine * sine * aqq;
        matrix[q][q] = sine * sine * app + 2.0 * sine * cosine * apq +
                       cosine * cosine * aqq;
        matrix[p][q] = matrix[q][p] = 0.0;

        for (std::size_t row = 0; row < dimensions; ++row) {
            const double vip = eigenvectors[row][p];
            const double viq = eigenvectors[row][q];
            eigenvectors[row][p] = cosine * vip - sine * viq;
            eigenvectors[row][q] = sine * vip + cosine * viq;
        }
    }

    square_roots.resize(dimensions);
    double smallest = std::numeric_limits<double>::infinity();
    double largest = 0.0;
    for (std::size_t index = 0; index < dimensions; ++index) {
        const double eigenvalue = std::max(matrix[index][index], eigenvalue_floor);
        smallest = std::min(smallest, eigenvalue);
        largest = std::max(largest, eigenvalue);
        square_roots[index] = std::sqrt(eigenvalue);
    }
    condition_number = largest / smallest;

    inverse_square_root.assign(dimensions, Vector(dimensions, 0.0));
    for (std::size_t row = 0; row < dimensions; ++row) {
        for (std::size_t column = 0; column < dimensions; ++column) {
            for (std::size_t axis = 0; axis < dimensions; ++axis) {
                inverse_square_root[row][column] +=
                    eigenvectors[row][axis] *
                    (1.0 / square_roots[axis]) *
                    eigenvectors[column][axis];
            }
        }
    }
}

}  // namespace

EvolutionStrategyOptimizer::EvolutionStrategyOptimizer(
    optimization::Bounds bounds,
    EsConfig config)
    : bounds_(std::move(bounds)), config_(config) {
    validate_es_config(bounds_, config_);
}

optimization::Result EvolutionStrategyOptimizer::optimize(
    const optimization::Objective& objective,
    const std::vector<optimization::Vector>& initial_positions,
    const optimization::Callback& callback) const {
    if (!objective) {
        throw std::invalid_argument("Objective function must not be empty");
    }
    const auto started = std::chrono::steady_clock::now();
    std::mt19937_64 random(config_.common.seed);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::normal_distribution<double> normal(0.0, 1.0);
    std::uniform_int_distribution<std::size_t> choose_parent(0, config_.parent_count - 1);

    std::vector<Candidate> parents(config_.parent_count);
    Result result;
    for (std::size_t index = 0; index < parents.size(); ++index) {
        parents[index].position.resize(bounds_.size());
        for (double& coordinate : parents[index].position) {
            coordinate = unit(random);
        }
        if (index < initial_positions.size()) {
            parents[index].position = normalize_position(initial_positions[index], bounds_);
        }
        parents[index].value = evaluate(objective, parents[index].position, bounds_);
        ++result.evaluations;
    }
    sort_candidates(parents, config_.common.maximize);
    result.best_position = denormalize_position(parents.front().position, bounds_);
    result.best_value = parents.front().value;
    result.history.push_back(result.best_value);

    double step_size = config_.initial_step_size;
    std::size_t stall_count = 0;
    std::size_t adaptation_successes = 0;
    std::size_t adaptation_trials = 0;

    for (std::size_t generation = 1;
         generation <= config_.common.max_generations;
         ++generation) {
        std::vector<Candidate> offspring;
        offspring.reserve(config_.offspring_count);
        for (std::size_t child_index = 0;
             child_index < config_.offspring_count;
             ++child_index) {
            const std::size_t first_parent = choose_parent(random);
            std::size_t second_parent = choose_parent(random);
            while (second_parent == first_parent) {
                second_parent = choose_parent(random);
            }

            Candidate child;
            child.position.resize(bounds_.size());
            for (std::size_t dimension = 0; dimension < bounds_.size(); ++dimension) {
                const double center = config_.recombination == Recombination::intermediate
                    ? 0.5 * (parents[first_parent].position[dimension] +
                             parents[second_parent].position[dimension])
                    : (unit(random) < 0.5
                        ? parents[first_parent].position[dimension]
                        : parents[second_parent].position[dimension]);
                child.position[dimension] = reflect_unit(center + step_size * normal(random));
            }
            child.value = evaluate(objective, child.position, bounds_);
            ++result.evaluations;
            ++adaptation_trials;
            if (better(
                    child.value,
                    parents[first_parent].value,
                    config_.common.maximize)) {
                ++adaptation_successes;
            }
            offspring.push_back(std::move(child));
        }

        if (config_.selection_mode == SelectionMode::plus) {
            offspring.insert(offspring.end(), parents.begin(), parents.end());
        }
        sort_candidates(offspring, config_.common.maximize);
        offspring.resize(config_.parent_count);
        parents = std::move(offspring);

        if (config_.adapt_step_size &&
            generation % config_.adaptation_interval == 0) {
            const double success_rate = static_cast<double>(adaptation_successes) /
                                        static_cast<double>(adaptation_trials);
            if (success_rate > 0.2) {
                step_size *= config_.adaptation_factor;
            } else if (success_rate < 0.2) {
                step_size /= config_.adaptation_factor;
            }
            step_size = std::clamp(
                step_size, config_.min_step_size, config_.max_step_size);
            adaptation_successes = 0;
            adaptation_trials = 0;
        }

        update_result(
            parents.front(),
            config_.common,
            generation,
            stall_count,
            result,
            bounds_,
            callback);
        if (stall_count >= config_.common.stall_generations) {
            result.converged = true;
            break;
        }
    }

    result.elapsed_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    return result;
}

CmaEvolutionStrategyOptimizer::CmaEvolutionStrategyOptimizer(
    optimization::Bounds bounds,
    CmaEsConfig config)
    : bounds_(std::move(bounds)), config_(config) {
    static_cast<void>(validate_cma_config(bounds_, config_));
}

optimization::Result CmaEvolutionStrategyOptimizer::optimize(
    const optimization::Objective& objective,
    const std::vector<optimization::Vector>& initial_positions,
    const optimization::Callback& callback) const {
    if (!objective) {
        throw std::invalid_argument("Objective function must not be empty");
    }
    const auto started = std::chrono::steady_clock::now();
    const std::size_t dimensions = bounds_.size();
    const ResolvedCmaParameters parameters = validate_cma_config(bounds_, config_);
    std::mt19937_64 random(config_.common.seed);
    std::normal_distribution<double> normal(0.0, 1.0);

    Result result;
    Vector mean(dimensions, 0.5);
    if (initial_positions.empty()) {
        result.best_position = denormalize_position(mean, bounds_);
        result.best_value = evaluate(objective, mean, bounds_);
        result.evaluations = 1;
    } else {
        result.best_value = config_.common.maximize
            ? -std::numeric_limits<double>::infinity()
            : std::numeric_limits<double>::infinity();
        for (const Vector& initial : initial_positions) {
            const Vector normalized = normalize_position(initial, bounds_);
            const double value = evaluate(objective, normalized, bounds_);
            ++result.evaluations;
            if (better(value, result.best_value, config_.common.maximize)) {
                result.best_value = value;
                result.best_position = denormalize_position(normalized, bounds_);
                mean = normalized;
            }
        }
    }
    result.history.push_back(result.best_value);

    double sigma = config_.initial_step_size;
    Matrix covariance = identity_matrix(dimensions);
    Matrix eigenvectors = identity_matrix(dimensions);
    Matrix inverse_square_root = identity_matrix(dimensions);
    Vector axis_scales(dimensions, 1.0);
    Vector covariance_path(dimensions, 0.0);
    Vector sigma_path(dimensions, 0.0);
    std::size_t stall_count = 0;
    double condition_number = 1.0;

    for (std::size_t generation = 1;
         generation <= config_.common.max_generations;
         ++generation) {
        std::vector<CmaCandidate> offspring;
        offspring.reserve(parameters.lambda);
        for (std::size_t sample = 0; sample < parameters.lambda; ++sample) {
            Vector standard_normal(dimensions);
            for (double& coordinate : standard_normal) {
                coordinate = normal(random);
            }

            Vector scaled_normal(dimensions);
            for (std::size_t axis = 0; axis < dimensions; ++axis) {
                scaled_normal[axis] = axis_scales[axis] * standard_normal[axis];
            }
            Vector normalized_step = matrix_vector_product(eigenvectors, scaled_normal);
            Vector position(dimensions);
            for (std::size_t dimension = 0; dimension < dimensions; ++dimension) {
                position[dimension] = reflect_unit(
                    mean[dimension] + sigma * normalized_step[dimension]);
                // 使用边界修复后的真实步长更新协方差，避免学习一个实际没有执行的越界步长。
                normalized_step[dimension] =
                    (position[dimension] - mean[dimension]) / sigma;
            }
            const double value = evaluate(objective, position, bounds_);
            ++result.evaluations;
            offspring.push_back({std::move(position), std::move(normalized_step), value});
        }

        std::sort(offspring.begin(), offspring.end(), [&](const CmaCandidate& left,
                                                          const CmaCandidate& right) {
            return better(left.value, right.value, config_.common.maximize);
        });

        const Vector old_mean = mean;
        std::fill(mean.begin(), mean.end(), 0.0);
        for (std::size_t parent = 0; parent < parameters.mu; ++parent) {
            for (std::size_t dimension = 0; dimension < dimensions; ++dimension) {
                mean[dimension] +=
                    parameters.weights[parent] * offspring[parent].position[dimension];
            }
        }

        Vector weighted_step(dimensions);
        for (std::size_t dimension = 0; dimension < dimensions; ++dimension) {
            weighted_step[dimension] = (mean[dimension] - old_mean[dimension]) / sigma;
        }
        const Vector whitened_step = matrix_vector_product(
            inverse_square_root, weighted_step);
        const double sigma_path_scale = std::sqrt(
            parameters.c_sigma * (2.0 - parameters.c_sigma) * parameters.mu_effective);
        for (std::size_t dimension = 0; dimension < dimensions; ++dimension) {
            sigma_path[dimension] =
                (1.0 - parameters.c_sigma) * sigma_path[dimension] +
                sigma_path_scale * whitened_step[dimension];
        }

        const double sigma_path_norm = vector_norm(sigma_path);
        const double path_normalizer = std::sqrt(
            1.0 - std::pow(1.0 - parameters.c_sigma, 2.0 * generation));
        const bool h_sigma = sigma_path_norm /
            (path_normalizer * parameters.expected_normal_length) <
            1.4 + 2.0 / (static_cast<double>(dimensions) + 1.0);
        const double covariance_path_scale = h_sigma
            ? std::sqrt(parameters.c_c * (2.0 - parameters.c_c) *
                        parameters.mu_effective)
            : 0.0;
        for (std::size_t dimension = 0; dimension < dimensions; ++dimension) {
            covariance_path[dimension] =
                (1.0 - parameters.c_c) * covariance_path[dimension] +
                covariance_path_scale * weighted_step[dimension];
        }

        const Matrix old_covariance = covariance;
        const double old_covariance_factor =
            1.0 - parameters.c1 - parameters.c_mu +
            (h_sigma ? 0.0 : parameters.c1 * parameters.c_c * (2.0 - parameters.c_c));
        for (std::size_t row = 0; row < dimensions; ++row) {
            for (std::size_t column = 0; column < dimensions; ++column) {
                double rank_mu = 0.0;
                for (std::size_t parent = 0; parent < parameters.mu; ++parent) {
                    rank_mu += parameters.weights[parent] *
                        offspring[parent].normalized_step[row] *
                        offspring[parent].normalized_step[column];
                }
                covariance[row][column] =
                    old_covariance_factor * old_covariance[row][column] +
                    parameters.c1 * covariance_path[row] * covariance_path[column] +
                    parameters.c_mu * rank_mu;
            }
        }

        // 强制恢复完全对称，消除累计浮点误差。
        for (std::size_t row = 0; row < dimensions; ++row) {
            for (std::size_t column = row + 1; column < dimensions; ++column) {
                const double symmetric = 0.5 *
                    (covariance[row][column] + covariance[column][row]);
                covariance[row][column] = symmetric;
                covariance[column][row] = symmetric;
            }
        }

        const double exponent = std::clamp(
            parameters.c_sigma / parameters.d_sigma *
                (sigma_path_norm / parameters.expected_normal_length - 1.0),
            -5.0,
            5.0);
        sigma = std::clamp(
            sigma * std::exp(exponent),
            config_.min_step_size,
            config_.max_step_size);

        bool numerical_stop = false;
        if (generation % config_.eigen_update_period == 0) {
            symmetric_eigendecomposition(
                covariance,
                config_.eigenvalue_floor,
                eigenvectors,
                axis_scales,
                inverse_square_root,
                condition_number);
            numerical_stop = condition_number > config_.max_condition_number;
        }

        const Candidate generation_best{
            offspring.front().position,
            offspring.front().value,
        };
        update_result(
            generation_best,
            config_.common,
            generation,
            stall_count,
            result,
            bounds_,
            callback);
        if (stall_count >= config_.common.stall_generations || numerical_stop) {
            result.converged = true;
            break;
        }
    }

    result.elapsed_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    return result;
}

}  // namespace es
