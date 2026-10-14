#!/usr/bin/env bash
set -euo pipefail

# 对照只在 GitHub Actions 的隔离检出目录内运行，不访问设备或发布资产。
build_dir="${1:?请提供已配置的构建目录}"
baseline="9faac57452eed4bc61da1fb9ba4aba04dd08d9fd"
evidence_dir="${RUNNER_TEMP:?仅允许在 GitHub Actions 中运行}/agc-startup-regression"
mkdir -p "${evidence_dir}/saved"
files=(
  src/AGVC/AGC/AGCGroupManager.cc
  src/AGVC/AGC/include/AGCGroupManager.h
  test/support/FakeDataCenter.hpp
)
for file in "${files[@]}"; do
  mkdir -p "${evidence_dir}/saved/$(dirname "${file}")"
  cp "${file}" "${evidence_dir}/saved/${file}"
done
restore_sources() {
  for file in "${files[@]}"; do
    cp "${evidence_dir}/saved/${file}" "${file}"
  done
}
trap restore_sources EXIT

expect_failed_test() {
  local target="$1" filter="$2" evidence="$3"
  cmake --build "${build_dir}" --target "${target}" --parallel 2
  set +e
  GTEST_FILTER="${filter}" timeout 45s ctest --test-dir "${build_dir}" \
    -R "^${target}$" --output-on-failure 2>&1 | tee "${evidence}"
  local result="${PIPESTATUS[0]}"
  set -e
  # CTest 的普通断言失败为 8；超时、编译失败或测试未被选中均不能当作复现。
  if [[ "${result}" != 8 ]]; then
    echo "旧实现未按预期出现测试断言失败，退出码=${result}" >&2
    return 1
  fi
  IFS=':' read -r -a expected_tests <<< "${filter}"
  for test_name in "${expected_tests[@]}"; do
    if ! grep -F "[  FAILED  ] ${test_name}" "${evidence}" >/dev/null; then
      echo "旧实现日志缺少预期失败用例：${test_name}" >&2
      return 1
    fi
  done
}

echo "使用新增测试验证旧 Fake 空闲关闭问题"
git show "${baseline}:test/support/FakeDataCenter.hpp" > test/support/FakeDataCenter.hpp
expect_failed_test fakeDataCenterSubscription_test \
  FakeDataCenterSubscriptionTest.IdleSubscriptionReceivesLaterMeasurement \
  "${evidence_dir}/legacy-fake.log"
restore_sources

echo "使用新增测试验证旧 AGC 启动丢量测与旧快照覆盖问题"
git show "${baseline}:src/AGVC/AGC/AGCGroupManager.cc" > src/AGVC/AGC/AGCGroupManager.cc
git show "${baseline}:src/AGVC/AGC/include/AGCGroupManager.h" > src/AGVC/AGC/include/AGCGroupManager.h
expect_failed_test agcGroupManager_test \
  AgcGroupManagerTest.StartupSubscriptionSnapshotRecoversMeasurementBeforeRegistration:AgcGroupManagerTest.StartupSnapshotCannotOverwriteNewerSubscriptionMeasurement \
  "${evidence_dir}/legacy-agc.log"
restore_sources

echo "恢复修复实现，执行同一批复现测试与完整 Fake 生命周期测试"
cmake --build "${build_dir}" --target agcGroupManager_test fakeDataCenterSubscription_test --parallel 2
GTEST_FILTER='AgcGroupManagerTest.StartupSubscriptionSnapshotRecoversMeasurementBeforeRegistration:AgcGroupManagerTest.StartupSnapshotCannotOverwriteNewerSubscriptionMeasurement:FakeDataCenterSubscriptionTest.*' \
  timeout 150s ctest --test-dir "${build_dir}" -R '^(agcGroupManager_test|fakeDataCenterSubscription_test)$' \
  --output-on-failure --verbose 2>&1 | tee "${evidence_dir}/fixed.log"
echo "旧实现红灯与修复实现绿灯对照通过，继续完整架构回归"
