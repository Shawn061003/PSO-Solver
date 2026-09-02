# Modeling PSO Solver

一个整洁、可复用的 C++20 粒子群优化（PSO）求解器，面向数学建模竞赛中的有界、
无梯度、非线性优化问题。

仓库只包含两项核心能力：

- 通用粒子群优化器；
- 从命令行指定路径的 YAML 文件加载 PSO 参数和变量边界。

具体赛题的运动方程、评价函数、数据和输出格式应放在使用方项目中，不进入本仓库。

## 目录

```text
include/pso/pso.hpp       PSO 公共接口
include/pso/config.hpp    YAML 与 PsoSettings 公共接口
src/pso.cpp               PSO 实现
src/config.cpp            零依赖 YAML 配置实现
config/pso.yaml           通用配置模板
tests/                    PSO 和配置加载测试
docs/                     已完成赛题的使用记录
```

## Visual Studio + CMake 构建

请在 Visual Studio Developer PowerShell 中执行：

```powershell
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

以上命令中的路径都相对于仓库根目录；`build/` 已加入 `.gitignore`，不会把编译产物
提交到仓库。将仓库复制到其他设备后，只需进入仓库根目录即可原样执行。

## 从指定 YAML 加载参数

配置文件格式见 [config/pso.yaml](config/pso.yaml)：

```yaml
solver:
  swarm_size: 64
  max_iterations: 150
  inertia_start: 0.90
  inertia_end: 0.40
  cognitive: 1.70
  social: 1.70
  velocity_limit_ratio: 0.25
  boundary_damping: 0.50
  tolerance: 1.0e-8
  stall_iterations: 40
  seed: 2026
  maximize: false

bounds:
  lower: [-5.0, -5.0]
  upper: [5.0, 5.0]
```

使用方程序负责接收配置路径：

```cpp
#include "pso/config.hpp"
#include "pso/pso.hpp"

#include <string>

int main(int argc, char* argv[]) {
    if (argc != 3 || std::string(argv[1]) != "--config") {
        return 2;
    }

    const pso::PsoSettings settings = pso::load_pso_settings(argv[2]);
    const pso::ParticleSwarmOptimizer optimizer(
        settings.bounds, settings.solver);

    const auto result = optimizer.optimize([](const pso::Vector& x) {
        return x[0] * x[0] + x[1] * x[1];
    });
}
```

调用形式统一为：

```powershell
.\build\your_model.exe --config .\config\solver.yaml
```

相对路径以命令执行时的工作目录为基准，因此建议始终从使用方项目根目录运行程序。

## 设置目标函数

目标函数不写在 YAML 中，而是在使用方的 C++ 代码中定义，并传给 `optimize()`：

```cpp
const pso::Objective objective = [](const pso::Vector& x) {
    return x[0] * x[0] + x[1] * x[1];
};

const pso::PsoResult result = optimizer.optimize(objective);
```

上例求解 `x[0]^2 + x[1]^2` 的最小值，因此 YAML 中设置：

```yaml
solver:
  maximize: false
```

如果要求最大值，将 `maximize` 改为 `true`，目标函数本身正常返回待最大化的数值，
不需要额外添加负号：

```cpp
const pso::Objective objective = [](const pso::Vector& x) {
    return 10.0 - (x[0] - 2.0) * (x[0] - 2.0);
};
```

`x[i]` 的物理意义必须与 YAML 边界顺序一致。例如 `x[0]` 表示速度、`x[1]` 表示
角度、`x[2]` 表示时间时：

```yaml
bounds:
  # 速度、角度、时间
  lower: [70.0, 0.0, 0.0]
  upper: [140.0, 6.283185307, 10.0]
```

有约束问题可以在目标函数中加入罚值。下面要求 `x[0] + x[1] <= 5`，并求最小值：

```cpp
const pso::Objective objective = [](const pso::Vector& x) {
    const double value = x[0] * x[0] + x[1] * x[1];
    const double excess = x[0] + x[1] - 5.0;
    const double penalty = excess > 0.0 ? 1.0e6 * excess * excess : 0.0;
    return value + penalty;  // 最小化加罚值；最大化则减去罚值
};
```

目标函数需要模型或数据时，可以捕获预先构造的模型对象：

```cpp
Model model(model_config);
const pso::Objective objective = [&model](const pso::Vector& x) {
    return model.evaluate(x);
};
```

不要在目标函数内部重复读取文件，因为一次 PSO 求解通常会调用目标函数数千次。

## 接入新赛题

1. 在赛题项目中定义决策向量每一维的物理意义；
2. 在 YAML 的 `bounds.lower`、`bounds.upper` 中按相同顺序填写边界；
3. 将模型封装为 `double objective(const pso::Vector&)`；
4. 对不可行解返回罚函数值；
5. 调用 `optimize()` 并保存 `PsoResult`；
6. 用多个随机种子和独立算法验证最优值。

`optimize()` 还支持传入初始粒子和逐代回调，便于热启动、打印收敛过程或记录曲线。

## YAML 支持范围

为了保证比赛现场断网也能编译，配置读取器不依赖 `yaml-cpp`。它支持本仓库所需的
标准 YAML 子集：

- 缩进映射；
- 字符串、浮点数、无符号整数和布尔值；
- `[a, b, c]` 形式的行内数值数组；
- 行尾注释。

缺失键、重复键、类型错误、维数不一致和非法边界都会直接报错。

## 已验证案例

A196 曾用于验证求解器，但其业务代码已从仓库移除。建模接口、参数、结果和清理原因
记录在 [docs/A196_VALIDATION_EXAMPLE.md](docs/A196_VALIDATION_EXAMPLE.md)。
