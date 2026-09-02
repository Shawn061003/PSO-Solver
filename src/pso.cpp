#include "pso/pso.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
#include <stdexcept>

namespace pso {
namespace {

// 粒子的运行时状态。
// position/velocity 描述当前状态，best_* 保存该粒子从初始化至今见过的最好状态。
struct Particle {
    Vector position;
    Vector velocity;
    Vector best_position;
    double value{};
    double best_value{};
};

// 将“最大化”和“最小化”的比较逻辑统一到一个函数，避免主循环散落方向判断。
bool better(double left, double right, bool maximize) {
    return maximize ? left > right : left < right;
}

// 检查与维数无关的超参数。变量边界在优化器构造函数中单独检查。
void validate_config(const PsoConfig& config) {
    if (config.swarm_size < 2 || config.max_iterations == 0 ||
        config.stall_iterations == 0) {
        throw std::invalid_argument("PSO sizes and iteration counts must be positive");
    }
    if (!(config.velocity_limit_ratio > 0.0 && config.velocity_limit_ratio <= 1.0)) {
        throw std::invalid_argument("velocity_limit_ratio must be in (0, 1]");
    }
    if (!(config.boundary_damping >= 0.0 && config.boundary_damping <= 1.0)) {
        throw std::invalid_argument("boundary_damping must be in [0, 1]");
    }
}

}  // namespace

ParticleSwarmOptimizer::ParticleSwarmOptimizer(Bounds bounds, PsoConfig config)
    : bounds_(std::move(bounds)), config_(config) {
    // 配置问题在正式计算前立即失败，避免运行几百代后才发现输入错误。
    validate_config(config_);
    if (bounds_.empty()) {
        throw std::invalid_argument("PSO needs at least one dimension");
    }
    for (const auto& [lower, upper] : bounds_) {
        if (!std::isfinite(lower) || !std::isfinite(upper) || lower >= upper) {
            throw std::invalid_argument("Each finite lower bound must be less than its upper bound");
        }
    }
}

PsoResult ParticleSwarmOptimizer::optimize(
    const Objective& objective,
    const std::vector<Vector>& initial_positions,
    const Callback& callback) const {
    const auto started = std::chrono::steady_clock::now();
    const std::size_t dimensions = bounds_.size();

    // mt19937_64 与固定 seed 配合，使实验可以复现。unit 同时用于位置、速度和
    // PSO 公式中的两个随机系数 r1、r2。
    std::mt19937_64 random(config_.seed);
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    std::vector<Particle> swarm(config_.swarm_size);
    Vector global_position(dimensions);

    // 初始全局最优值取“最差的无穷值”，这样第一个合法粒子一定能够替换它。
    double global_value = config_.maximize
        ? -std::numeric_limits<double>::infinity()
        : std::numeric_limits<double>::infinity();

    // 第 0 阶段：初始化整个粒子群，并完成每个粒子的第一次目标函数评估。
    for (std::size_t particle_index = 0; particle_index < swarm.size(); ++particle_index) {
        auto& particle = swarm[particle_index];
        particle.position.resize(dimensions);
        particle.velocity.resize(dimensions);
        for (std::size_t dimension = 0; dimension < dimensions; ++dimension) {
            const auto [lower, upper] = bounds_[dimension];
            const double span = upper - lower;

            // 位置覆盖完整可行域；初速度在 [-vmax, vmax] 中均匀产生。
            particle.position[dimension] = lower + unit(random) * span;
            particle.velocity[dimension] =
                (2.0 * unit(random) - 1.0) * config_.velocity_limit_ratio * span;
        }

        // 调用方提供的热启动粒子按顺序覆盖前若干个随机粒子，其余粒子仍负责全局探索。
        if (particle_index < initial_positions.size()) {
            if (initial_positions[particle_index].size() != dimensions) {
                throw std::invalid_argument("Initial particle has the wrong dimension");
            }
            for (std::size_t dimension = 0; dimension < dimensions; ++dimension) {
                // 裁剪能够容忍由近似计算产生的微小越界，但调用方仍应尽量提供可行点。
                particle.position[dimension] = std::clamp(
                    initial_positions[particle_index][dimension],
                    bounds_[dimension].first,
                    bounds_[dimension].second);
            }
        }

        // 初始化时当前位置就是该粒子的个体历史最优位置 pbest。
        particle.value = objective(particle.position);
        if (std::isnan(particle.value)) {
            throw std::runtime_error("Objective returned NaN");
        }
        particle.best_value = particle.value;
        particle.best_position = particle.position;
        if (better(particle.value, global_value, config_.maximize)) {
            global_value = particle.value;
            global_position = particle.position;
        }
    }

    PsoResult result;
    result.evaluations = swarm.size();
    result.history.push_back(global_value);
    std::size_t stall_count = 0;

    // 第 1 阶段：逐代更新速度、位置、个体最优 pbest 和群体最优 gbest。
    for (std::size_t iteration = 1; iteration <= config_.max_iterations; ++iteration) {
        // 惯性权重按代数线性退火：前期更敢探索，后期逐渐稳定。
        const double fraction = static_cast<double>(iteration) /
                                static_cast<double>(config_.max_iterations);
        const double inertia = config_.inertia_start +
            fraction * (config_.inertia_end - config_.inertia_start);

        for (auto& particle : swarm) {
            for (std::size_t dimension = 0; dimension < dimensions; ++dimension) {
                const auto [lower, upper] = bounds_[dimension];
                const double vmax = config_.velocity_limit_ratio * (upper - lower);

                // 经典 PSO 速度公式：
                // v = w*v + c1*r1*(pbest-x) + c2*r2*(gbest-x)
                // 三项依次代表运动惯性、个体经验和群体经验。
                particle.velocity[dimension] =
                    inertia * particle.velocity[dimension] +
                    config_.cognitive * unit(random) *
                        (particle.best_position[dimension] - particle.position[dimension]) +
                    config_.social * unit(random) *
                        (global_position[dimension] - particle.position[dimension]);

                // 限速以各维区间宽度为尺度，避免量纲不同的变量共用一个绝对速度上限。
                particle.velocity[dimension] = std::clamp(
                    particle.velocity[dimension], -vmax, vmax);
                particle.position[dimension] += particle.velocity[dimension];

                // 反射边界：越过下界多少就从下界向内退回多少，并让速度反向衰减。
                // 与简单截断相比，反射能保留部分运动趋势，减少粒子长期粘在边界上。
                if (particle.position[dimension] < lower) {
                    particle.position[dimension] = lower +
                        (lower - particle.position[dimension]);
                    particle.velocity[dimension] *= -config_.boundary_damping;
                }
                if (particle.position[dimension] > upper) {
                    particle.position[dimension] = upper -
                        (particle.position[dimension] - upper);
                    particle.velocity[dimension] *= -config_.boundary_damping;
                }

                // 一次更新可能跨越整个区间，反射一次后仍可能越界；最终裁剪作为安全兜底。
                particle.position[dimension] = std::clamp(
                    particle.position[dimension], lower, upper);
            }

            // 每个粒子每代只评估一次目标函数，并及时更新自己的历史最优。
            particle.value = objective(particle.position);
            ++result.evaluations;
            if (std::isnan(particle.value)) {
                throw std::runtime_error("Objective returned NaN");
            }
            if (better(particle.value, particle.best_value, config_.maximize)) {
                particle.best_value = particle.value;
                particle.best_position = particle.position;
            }
        }

        // 从所有 pbest 中找本代的候选 gbest。
        // max_element 的比较器按 maximize 调整方向，所以同一段代码兼容最大化和最小化。
        const auto best_iterator = std::max_element(
            swarm.begin(), swarm.end(), [&](const Particle& left, const Particle& right) {
                return better(right.best_value, left.best_value, config_.maximize);
            });
        const double candidate = best_iterator->best_value;
        if (better(candidate, global_value, config_.maximize)) {
            const double improvement = std::abs(candidate - global_value);
            global_value = candidate;
            global_position = best_iterator->best_position;
            // 很小的数值抖动不应无限延长搜索；只有超过 tolerance 才清零停滞计数。
            stall_count = improvement > config_.tolerance ? 0 : stall_count + 1;
        } else {
            ++stall_count;
        }

        result.iterations = iteration;
        result.history.push_back(global_value);

        // 回调发生在本代所有状态更新完成之后，观察到的是一致的代末状态。
        if (callback) {
            callback(iteration, global_value, global_position);
        }
        if (stall_count >= config_.stall_iterations) {
            // converged 表示满足本实现的停滞终止准则，不等价于数学上证明了全局最优。
            result.converged = true;
            break;
        }
    }

    // 汇总最终结果。history 的长度始终等于 iterations + 1（包含初始化状态）。
    result.best_position = std::move(global_position);
    result.best_value = global_value;
    result.elapsed_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    return result;
}

}  // namespace pso
