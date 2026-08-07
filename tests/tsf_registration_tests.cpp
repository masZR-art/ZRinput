#include "test_harness.h"
#include "windows/tsf/registration_transaction.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using zrinput::windows::tsf::ComRegistrationSnapshot;
using zrinput::windows::tsf::IsComRegistrationOwnedBy;
using zrinput::windows::tsf::RawRegistryValue;
using zrinput::windows::tsf::RegisterWithBackend;
using zrinput::windows::tsf::RegistrationBackend;
using zrinput::windows::tsf::RegistrationState;
using zrinput::windows::tsf::UnregisterWithBackend;
using zrinput::windows::tsf::kRegistrationCategoryCount;

constexpr std::wstring_view kModulePath =
    L"C:\\Users\\Test\\ZRinput\\ZRinputTsf.dll";
constexpr HRESULT kPrimaryFailure =
    MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x701);
constexpr HRESULT kRollbackFailure =
    MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x702);
constexpr HRESULT kSecondFailure =
    MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x703);

enum class Step : std::size_t {
  kCapture,
  kWriteCom,
  kRestoreCom,
  kRemoveCom,
  kAddProcessor,
  kRemoveProcessor,
  kAddProfile,
  kRemoveProfile,
  kAddCategory0,
  kAddCategory1,
  kAddCategory2,
  kRemoveCategory0,
  kRemoveCategory1,
  kRemoveCategory2,
  kCount,
};

Step AddCategoryStep(std::size_t index) {
  return static_cast<Step>(static_cast<std::size_t>(Step::kAddCategory0) +
                           index);
}

Step RemoveCategoryStep(std::size_t index) {
  return static_cast<Step>(static_cast<std::size_t>(Step::kRemoveCategory0) +
                           index);
}

RawRegistryValue RegistryString(std::wstring_view text) {
  RawRegistryValue value;
  value.existed = true;
  value.type = REG_SZ;
  value.bytes.assign((text.size() + 1) * sizeof(wchar_t), std::byte{0});
  if (!text.empty()) {
    std::memcpy(value.bytes.data(), text.data(), text.size() * sizeof(wchar_t));
  }
  return value;
}

ComRegistrationSnapshot DesiredCom(std::wstring_view path = kModulePath) {
  ComRegistrationSnapshot snapshot;
  snapshot.clsid_key_existed = true;
  snapshot.inproc_key_existed = true;
  snapshot.default_value = RegistryString(path);
  snapshot.threading_model = RegistryString(L"Apartment");
  return snapshot;
}

RegistrationState FullState(std::wstring_view path = kModulePath) {
  RegistrationState state;
  state.com = DesiredCom(path);
  state.processor = true;
  state.profile = true;
  state.categories.fill(true);
  return state;
}

struct FailureRule {
  std::size_t fail_on_call = 0;
  std::size_t calls = 0;
  HRESULT result = E_FAIL;
  bool after_mutation = false;
};

class FakeBackend final : public RegistrationBackend {
 public:
  FakeBackend() = default;
  explicit FakeBackend(RegistrationState initial)
      : state(std::move(initial)) {}

  void Fail(Step step,
            HRESULT result,
            bool after_mutation = false,
            std::size_t call = 1) {
    auto& rule = rules_[Index(step)];
    rule.fail_on_call = call;
    rule.result = result;
    rule.after_mutation = after_mutation;
  }

  void OverrideCapture(std::size_t call, RegistrationState captured) {
    capture_override_call_ = call;
    capture_override_ = std::move(captured);
  }

  void ThrowAfterComWriteSideEffect() noexcept {
    throw_after_com_write_side_effect_ = true;
  }

  [[nodiscard]] std::size_t Calls(Step step) const {
    return static_cast<std::size_t>(
        std::count(log.begin(), log.end(), step));
  }

  HRESULT CaptureState(RegistrationState* captured) override {
    if (captured == nullptr) {
      return E_POINTER;
    }
    return Mutate(Step::kCapture, [&] {
      const std::size_t call = Calls(Step::kCapture);
      *captured = capture_override_.has_value() &&
                          call == capture_override_call_
                      ? *capture_override_
                      : state;
    });
  }

  HRESULT WriteComRegistration(
      std::wstring_view path,
      ComRegistrationSnapshot* previous,
      bool* changed) override {
    if (previous == nullptr || changed == nullptr) {
      return E_POINTER;
    }
    *previous = state.com;
    *changed = false;
    const ComRegistrationSnapshot desired = DesiredCom(path);
    if (state.com == desired) {
      return Mutate(Step::kWriteCom, [] {});
    }
    return Mutate(Step::kWriteCom, [&] {
      *changed = true;
      state.com = desired;
      if (throw_after_com_write_side_effect_) {
        throw std::bad_alloc();
      }
    });
  }

  HRESULT RestoreComRegistration(
      const ComRegistrationSnapshot& snapshot) override {
    return Mutate(Step::kRestoreCom, [&] { state.com = snapshot; });
  }

  HRESULT RemoveComRegistrationIfOwned(std::wstring_view path) override {
    log.push_back(Step::kRemoveCom);
    auto& rule = rules_[Index(Step::kRemoveCom)];
    ++rule.calls;
    const bool fail = rule.fail_on_call == rule.calls;
    if (fail && !rule.after_mutation) {
      return rule.result;
    }
    bool owned = false;
    const HRESULT ownership = IsComRegistrationOwnedBy(state.com, path, &owned);
    if (FAILED(ownership)) {
      return ownership;
    }
    if (!owned) {
      return HRESULT_FROM_WIN32(ERROR_NOT_OWNER);
    }
    state.com = {};
    return fail ? rule.result : S_OK;
  }

  HRESULT AddProcessor(bool* created) override {
    if (created == nullptr) {
      return E_POINTER;
    }
    *created = false;
    if (state.processor) {
      return Mutate(Step::kAddProcessor, [] {});
    }
    return Mutate(Step::kAddProcessor, [&] {
      state.processor = true;
      *created = true;
    });
  }

  HRESULT RemoveProcessor() override {
    return Mutate(Step::kRemoveProcessor, [&] { state.processor = false; });
  }

  HRESULT AddProfile(std::wstring_view, bool* created) override {
    if (created == nullptr) {
      return E_POINTER;
    }
    *created = false;
    if (state.profile) {
      return Mutate(Step::kAddProfile, [] {});
    }
    return Mutate(Step::kAddProfile, [&] {
      state.profile = true;
      *created = true;
    });
  }

  HRESULT RemoveProfile() override {
    return Mutate(Step::kRemoveProfile, [&] { state.profile = false; });
  }

  HRESULT AddCategory(std::size_t index, bool* created) override {
    if (index >= state.categories.size()) {
      return E_INVALIDARG;
    }
    if (created == nullptr) {
      return E_POINTER;
    }
    *created = false;
    if (state.categories[index]) {
      return Mutate(AddCategoryStep(index), [] {});
    }
    return Mutate(AddCategoryStep(index), [&] {
      state.categories[index] = true;
      *created = true;
    });
  }

  HRESULT RemoveCategory(std::size_t index) override {
    if (index >= state.categories.size()) {
      return E_INVALIDARG;
    }
    return Mutate(RemoveCategoryStep(index),
                  [&] { state.categories[index] = false; });
  }

  RegistrationState state;
  std::vector<Step> log;

 private:
  static constexpr std::size_t Index(Step step) {
    return static_cast<std::size_t>(step);
  }

  template <typename Mutation>
  HRESULT Mutate(Step step, Mutation&& mutation) {
    log.push_back(step);
    auto& rule = rules_[Index(step)];
    ++rule.calls;
    const bool fail = rule.fail_on_call == rule.calls;
    if (fail && !rule.after_mutation) {
      return rule.result;
    }
    mutation();
    return fail ? rule.result : S_OK;
  }

  std::array<FailureRule, static_cast<std::size_t>(Step::kCount)> rules_{};
  std::size_t capture_override_call_ = 0;
  std::optional<RegistrationState> capture_override_;
  bool throw_after_com_write_side_effect_ = false;
};

ZR_TEST(RegisterCleanStateCreatesCompleteRegistration) {
  FakeBackend backend;
  ZR_EXPECT_EQ(RegisterWithBackend(backend, kModulePath), S_OK);
  ZR_EXPECT_EQ(backend.state, FullState());
  ZR_EXPECT_EQ(backend.Calls(Step::kCapture), std::size_t{2});
  ZR_EXPECT_EQ(backend.Calls(Step::kWriteCom), std::size_t{1});
  ZR_EXPECT_EQ(backend.Calls(Step::kAddProcessor), std::size_t{1});
  ZR_EXPECT_EQ(backend.Calls(Step::kAddProfile), std::size_t{1});
  for (std::size_t index = 0; index < kRegistrationCategoryCount; ++index) {
    ZR_EXPECT_EQ(backend.Calls(AddCategoryStep(index)), std::size_t{1});
  }
  ZR_EXPECT_EQ(backend.Calls(Step::kRestoreCom), std::size_t{0});
}

ZR_TEST(RegisterPreflightFailurePerformsNoMutation) {
  const RegistrationState initial = FullState(L"C:\\Old\\ZRinputTsf.dll");
  FakeBackend backend(initial);
  backend.Fail(Step::kCapture, E_ACCESSDENIED);
  ZR_EXPECT_EQ(RegisterWithBackend(backend, kModulePath), E_ACCESSDENIED);
  ZR_EXPECT_EQ(backend.state, initial);
  ZR_EXPECT_EQ(backend.log, std::vector<Step>{Step::kCapture});
}

ZR_TEST(MalformedPreflightSnapshotPerformsNoMutation) {
  RegistrationState initial;
  initial.com.inproc_key_existed = true;
  FakeBackend register_backend(initial);
  ZR_EXPECT_EQ(RegisterWithBackend(register_backend, kModulePath),
               HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
  ZR_EXPECT_EQ(register_backend.state, initial);
  ZR_EXPECT_EQ(register_backend.log,
               std::vector<Step>{Step::kCapture});

  FakeBackend unregister_backend(initial);
  ZR_EXPECT_EQ(UnregisterWithBackend(unregister_backend, kModulePath),
               HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
  ZR_EXPECT_EQ(unregister_backend.state, initial);
  ZR_EXPECT_EQ(unregister_backend.log,
               std::vector<Step>{Step::kCapture});
}

ZR_TEST(RegisterPreservesAllPartialTsfPresenceCombinations) {
  constexpr std::size_t kStateBits = 2 + kRegistrationCategoryCount;
  for (std::size_t mask = 0; mask < (std::size_t{1} << kStateBits); ++mask) {
    RegistrationState initial;
    initial.com = DesiredCom(L"C:\\Old\\ZRinputTsf.dll");
    initial.processor = (mask & 1u) != 0;
    initial.profile = (mask & 2u) != 0;
    for (std::size_t index = 0; index < kRegistrationCategoryCount; ++index) {
      initial.categories[index] = (mask & (std::size_t{4} << index)) != 0;
    }
    FakeBackend backend(initial);
    ZR_EXPECT_EQ(RegisterWithBackend(backend, kModulePath), S_OK);
    ZR_EXPECT_EQ(backend.state, FullState());
    ZR_EXPECT_EQ(backend.Calls(Step::kAddProcessor),
                 initial.processor ? std::size_t{0} : std::size_t{1});
    ZR_EXPECT_EQ(backend.Calls(Step::kAddProfile),
                 initial.profile ? std::size_t{0} : std::size_t{1});
    for (std::size_t index = 0; index < kRegistrationCategoryCount; ++index) {
      ZR_EXPECT_EQ(backend.Calls(AddCategoryStep(index)),
                   initial.categories[index] ? std::size_t{0}
                                             : std::size_t{1});
    }
  }
}

ZR_TEST(RegistrationFailureRestoresExactComKeysTypesAndBytes) {
  ComRegistrationSnapshot clsid_absent;

  ComRegistrationSnapshot inproc_absent;
  inproc_absent.clsid_key_existed = true;

  ComRegistrationSnapshot values_absent;
  values_absent.clsid_key_existed = true;
  values_absent.inproc_key_existed = true;

  ComRegistrationSnapshot arbitrary_values = values_absent;
  arbitrary_values.default_value.existed = true;
  arbitrary_values.default_value.type = REG_EXPAND_SZ;
  arbitrary_values.default_value.bytes = {
      std::byte{0x25}, std::byte{0x00}, std::byte{0x58}, std::byte{0x00}};
  arbitrary_values.threading_model.existed = true;
  arbitrary_values.threading_model.type = REG_BINARY;
  arbitrary_values.threading_model.bytes = {
      std::byte{0x00}, std::byte{0x7f}, std::byte{0xff}};

  ComRegistrationSnapshot default_only = values_absent;
  default_only.default_value = arbitrary_values.default_value;

  ComRegistrationSnapshot threading_only = values_absent;
  threading_only.threading_model = arbitrary_values.threading_model;

  const std::array variants = {clsid_absent, inproc_absent, values_absent,
                               default_only, threading_only,
                               arbitrary_values};
  for (const auto& com : variants) {
    RegistrationState initial;
    initial.com = com;
    FakeBackend backend(initial);
    backend.Fail(Step::kAddProcessor, kPrimaryFailure);
    ZR_EXPECT_EQ(RegisterWithBackend(backend, kModulePath), kPrimaryFailure);
    ZR_EXPECT_EQ(backend.state, initial);
  }
}

ZR_TEST(EveryRegistrationMutationFailureRollsBackBeforeAndAfterSideEffect) {
  const std::array steps = {
      Step::kWriteCom,      Step::kAddProcessor, Step::kAddProfile,
      Step::kAddCategory0, Step::kAddCategory1, Step::kAddCategory2,
  };
  for (const Step step : steps) {
    for (const bool after_mutation : {false, true}) {
      FakeBackend backend;
      backend.Fail(step, kPrimaryFailure, after_mutation);
      ZR_EXPECT_EQ(RegisterWithBackend(backend, kModulePath), kPrimaryFailure);
      ZR_EXPECT_EQ(backend.state, RegistrationState{});
      const std::size_t expected_restore_calls =
          step == Step::kWriteCom && !after_mutation ? std::size_t{0}
                                                     : std::size_t{1};
      ZR_EXPECT_EQ(backend.Calls(Step::kRestoreCom), expected_restore_calls);
    }
  }
}

ZR_TEST(RollbackRemovesOnlyItemsAbsentBeforeTheTransaction) {
  RegistrationState initial;
  initial.com = DesiredCom(L"C:\\Old\\ZRinputTsf.dll");
  initial.processor = true;
  initial.profile = true;
  initial.categories[0] = true;
  FakeBackend backend(initial);
  backend.Fail(Step::kAddCategory2, kPrimaryFailure, true);
  ZR_EXPECT_EQ(RegisterWithBackend(backend, kModulePath), kPrimaryFailure);
  ZR_EXPECT_EQ(backend.state, initial);
  ZR_EXPECT_EQ(backend.Calls(Step::kRemoveProcessor), std::size_t{0});
  ZR_EXPECT_EQ(backend.Calls(Step::kRemoveProfile), std::size_t{0});
  ZR_EXPECT_EQ(backend.Calls(Step::kRemoveCategory0), std::size_t{0});
  ZR_EXPECT_EQ(backend.Calls(Step::kRemoveCategory1), std::size_t{1});
  ZR_EXPECT_EQ(backend.Calls(Step::kRemoveCategory2), std::size_t{1});
}

ZR_TEST(RollbackDoesNotRemoveResourcesThatAppearedAfterPreflight) {
  RegistrationState concurrent_state;
  concurrent_state.com = DesiredCom();
  concurrent_state.processor = true;
  concurrent_state.profile = true;
  concurrent_state.categories[0] = true;

  FakeBackend backend(concurrent_state);
  backend.OverrideCapture(1, RegistrationState{});
  backend.Fail(Step::kAddCategory1, kPrimaryFailure);

  ZR_EXPECT_EQ(RegisterWithBackend(backend, kModulePath), kPrimaryFailure);
  ZR_EXPECT_EQ(backend.state, concurrent_state);
  ZR_EXPECT_EQ(backend.Calls(Step::kAddProcessor), std::size_t{1});
  ZR_EXPECT_EQ(backend.Calls(Step::kAddProfile), std::size_t{1});
  ZR_EXPECT_EQ(backend.Calls(Step::kAddCategory0), std::size_t{1});
  ZR_EXPECT_EQ(backend.Calls(Step::kRemoveProcessor), std::size_t{0});
  ZR_EXPECT_EQ(backend.Calls(Step::kRemoveProfile), std::size_t{0});
  ZR_EXPECT_EQ(backend.Calls(Step::kRemoveCategory0), std::size_t{0});
  ZR_EXPECT_EQ(backend.Calls(Step::kRestoreCom), std::size_t{0});
}

ZR_TEST(RollbackUsesTheImmediatePrewriteComSnapshot) {
  RegistrationState concurrent_state;
  concurrent_state.com = DesiredCom(L"C:\\Concurrent\\ZRinputTsf.dll");

  FakeBackend backend(concurrent_state);
  backend.OverrideCapture(1, RegistrationState{});
  backend.Fail(Step::kAddProcessor, kPrimaryFailure);

  ZR_EXPECT_EQ(RegisterWithBackend(backend, kModulePath), kPrimaryFailure);
  ZR_EXPECT_EQ(backend.state, concurrent_state);
  ZR_EXPECT_EQ(backend.Calls(Step::kWriteCom), std::size_t{1});
  ZR_EXPECT_EQ(backend.Calls(Step::kRestoreCom), std::size_t{1});
}

ZR_TEST(ComWriteOwnershipSurvivesAnExceptionAfterItsSideEffect) {
  RegistrationState initial;
  initial.com = DesiredCom(L"C:\\Previous\\ZRinputTsf.dll");
  FakeBackend backend(initial);
  backend.ThrowAfterComWriteSideEffect();

  ZR_EXPECT_EQ(RegisterWithBackend(backend, kModulePath), E_OUTOFMEMORY);
  ZR_EXPECT_EQ(backend.state, initial);
  ZR_EXPECT_EQ(backend.Calls(Step::kWriteCom), std::size_t{1});
  ZR_EXPECT_EQ(backend.Calls(Step::kRestoreCom), std::size_t{1});
}

ZR_TEST(TsfRollbackFailuresKeepNewComForFreshInstallAndUpgrade) {
  const std::array tsf_rollback_steps = {
      Step::kRemoveCategory2, Step::kRemoveCategory1,
      Step::kRemoveCategory0, Step::kRemoveProfile,
      Step::kRemoveProcessor,
  };
  const std::array original_com_states = {
      ComRegistrationSnapshot{}, DesiredCom(L"C:\\Old\\ZRinputTsf.dll")};
  for (const auto& original_com : original_com_states) {
    for (const Step rollback_step : tsf_rollback_steps) {
      for (const bool after_mutation : {false, true}) {
        RegistrationState initial;
        initial.com = original_com;
        FakeBackend backend(initial);
        backend.Fail(Step::kAddCategory2, kPrimaryFailure, true);
        backend.Fail(rollback_step, kRollbackFailure, after_mutation);
        ZR_EXPECT_EQ(RegisterWithBackend(backend, kModulePath),
                     kRollbackFailure);
        for (std::size_t index = 0; index < kRegistrationCategoryCount;
             ++index) {
          ZR_EXPECT_EQ(backend.Calls(RemoveCategoryStep(index)),
                       std::size_t{1});
        }
        ZR_EXPECT_EQ(backend.Calls(Step::kRemoveProfile), std::size_t{1});
        ZR_EXPECT_EQ(
            backend.Calls(Step::kRemoveProcessor),
            rollback_step == Step::kRemoveProfile ? std::size_t{0}
                                                   : std::size_t{1});
        ZR_EXPECT_EQ(backend.Calls(Step::kRestoreCom), std::size_t{0});
        ZR_EXPECT_EQ(backend.state.com, DesiredCom());
      }
    }
  }
}

ZR_TEST(ComRestoreFailureIsPropagatedAfterSuccessfulTsfUndo) {
  const std::array original_com_states = {
      ComRegistrationSnapshot{}, DesiredCom(L"C:\\Old\\ZRinputTsf.dll")};
  for (const auto& original_com : original_com_states) {
    for (const bool after_mutation : {false, true}) {
      RegistrationState initial;
      initial.com = original_com;
      FakeBackend backend(initial);
      backend.Fail(Step::kAddCategory2, kPrimaryFailure, true);
      backend.Fail(Step::kRestoreCom, kRollbackFailure, after_mutation);
      ZR_EXPECT_EQ(RegisterWithBackend(backend, kModulePath),
                   kRollbackFailure);
      ZR_EXPECT_TRUE(!backend.state.processor);
      ZR_EXPECT_TRUE(!backend.state.profile);
      ZR_EXPECT_TRUE(std::ranges::none_of(
          backend.state.categories, [](bool value) { return value; }));
      ZR_EXPECT_EQ(backend.Calls(Step::kRestoreCom), std::size_t{1});
      ZR_EXPECT_EQ(backend.state.com,
                   after_mutation ? original_com : DesiredCom());
    }
  }
}

ZR_TEST(FirstRollbackFailureWinsWhileIndependentUndoContinues) {
  FakeBackend backend;
  backend.Fail(Step::kAddCategory2, kPrimaryFailure, true);
  backend.Fail(Step::kRemoveCategory2, kRollbackFailure);
  backend.Fail(Step::kRemoveCategory1, kSecondFailure);
  ZR_EXPECT_EQ(RegisterWithBackend(backend, kModulePath), kRollbackFailure);
  ZR_EXPECT_EQ(backend.Calls(Step::kRemoveCategory2), std::size_t{1});
  ZR_EXPECT_EQ(backend.Calls(Step::kRemoveCategory1), std::size_t{1});
  ZR_EXPECT_EQ(backend.Calls(Step::kRemoveCategory0), std::size_t{1});
  ZR_EXPECT_EQ(backend.Calls(Step::kRemoveProfile), std::size_t{1});
  ZR_EXPECT_EQ(backend.Calls(Step::kRemoveProcessor), std::size_t{1});
  ZR_EXPECT_EQ(backend.Calls(Step::kRestoreCom), std::size_t{0});
  ZR_EXPECT_EQ(backend.state.com, DesiredCom());
}

ZR_TEST(PostflightProbeFailureRollsBackTheCompletedMutationSet) {
  FakeBackend backend;
  backend.Fail(Step::kCapture, E_ACCESSDENIED, false, 2);
  ZR_EXPECT_EQ(RegisterWithBackend(backend, kModulePath), E_ACCESSDENIED);
  ZR_EXPECT_EQ(backend.state, RegistrationState{});
  ZR_EXPECT_EQ(backend.Calls(Step::kRestoreCom), std::size_t{1});
}

ZR_TEST(PostflightStateMismatchRollsBackTheCompletedMutationSet) {
  FakeBackend backend;
  RegistrationState incomplete = FullState();
  incomplete.categories[1] = false;
  backend.OverrideCapture(2, incomplete);
  ZR_EXPECT_EQ(RegisterWithBackend(backend, kModulePath), E_FAIL);
  ZR_EXPECT_EQ(backend.state, RegistrationState{});
  ZR_EXPECT_EQ(backend.Calls(Step::kRestoreCom), std::size_t{1});
}

ZR_TEST(OwnershipRequiresWellFormedRegSzAndMatchesCaseInsensitively) {
  bool owned = false;
  const auto desired = DesiredCom();
  ZR_EXPECT_EQ(IsComRegistrationOwnedBy(desired, kModulePath, nullptr),
               E_POINTER);
  owned = true;
  ZR_EXPECT_EQ(IsComRegistrationOwnedBy(desired, L"", &owned), E_INVALIDARG);
  ZR_EXPECT_TRUE(!owned);
  ZR_EXPECT_EQ(IsComRegistrationOwnedBy(desired, kModulePath, &owned), S_OK);
  ZR_EXPECT_TRUE(owned);
  ZR_EXPECT_EQ(IsComRegistrationOwnedBy(
                   desired, L"c:\\users\\test\\zrinput\\zrinputtsf.dll",
                   &owned),
               S_OK);
  ZR_EXPECT_TRUE(owned);

  auto wrong_path = DesiredCom(L"C:\\Other\\ZRinputTsf.dll");
  ZR_EXPECT_EQ(IsComRegistrationOwnedBy(wrong_path, kModulePath, &owned), S_OK);
  ZR_EXPECT_TRUE(!owned);

  auto wrong_type = desired;
  wrong_type.default_value.type = REG_EXPAND_SZ;
  ZR_EXPECT_EQ(IsComRegistrationOwnedBy(wrong_type, kModulePath, &owned),
               HRESULT_FROM_WIN32(ERROR_INVALID_DATA));

  auto missing_terminator = desired;
  missing_terminator.default_value.bytes.resize(
      missing_terminator.default_value.bytes.size() - sizeof(wchar_t));
  ZR_EXPECT_EQ(
      IsComRegistrationOwnedBy(missing_terminator, kModulePath, &owned),
      HRESULT_FROM_WIN32(ERROR_INVALID_DATA));

  auto embedded_null = desired;
  const wchar_t zero = L'\0';
  std::memcpy(embedded_null.default_value.bytes.data() + sizeof(wchar_t),
              &zero, sizeof(zero));
  ZR_EXPECT_EQ(IsComRegistrationOwnedBy(embedded_null, kModulePath, &owned),
               HRESULT_FROM_WIN32(ERROR_INVALID_DATA));

  auto odd_byte_count = desired;
  odd_byte_count.default_value.bytes.push_back(std::byte{0});
  ZR_EXPECT_EQ(IsComRegistrationOwnedBy(odd_byte_count, kModulePath, &owned),
               HRESULT_FROM_WIN32(ERROR_INVALID_DATA));

  auto inconsistent_shape = desired;
  inconsistent_shape.clsid_key_existed = false;
  ZR_EXPECT_EQ(
      IsComRegistrationOwnedBy(inconsistent_shape, kModulePath, &owned),
      HRESULT_FROM_WIN32(ERROR_INVALID_DATA));

  std::wstring embedded_path{kModulePath};
  embedded_path.insert(3, 1, L'\0');
  ZR_EXPECT_EQ(IsComRegistrationOwnedBy(desired, embedded_path, &owned),
               E_INVALIDARG);
}

ZR_TEST(UnregisterOwnedStateRemovesTsfBeforeCom) {
  FakeBackend backend(FullState());
  ZR_EXPECT_EQ(UnregisterWithBackend(backend, kModulePath), S_OK);
  ZR_EXPECT_EQ(backend.state, RegistrationState{});
  const std::vector expected = {
      Step::kCapture,         Step::kRemoveCategory0,
      Step::kRemoveCategory1, Step::kRemoveCategory2,
      Step::kRemoveProfile,   Step::kRemoveProcessor,
      Step::kCapture,         Step::kRemoveCom,
  };
  ZR_EXPECT_EQ(backend.log, expected);
}

ZR_TEST(UnregisterIsIdempotentWhenNothingIsRegistered) {
  FakeBackend backend;
  ZR_EXPECT_EQ(UnregisterWithBackend(backend, kModulePath), S_OK);
  ZR_EXPECT_EQ(backend.log, std::vector<Step>{Step::kCapture});

  FakeBackend installed(FullState());
  ZR_EXPECT_EQ(UnregisterWithBackend(installed, kModulePath), S_OK);
  ZR_EXPECT_EQ(UnregisterWithBackend(installed, kModulePath), S_OK);
  ZR_EXPECT_EQ(installed.state, RegistrationState{});
}

ZR_TEST(UnregisterFailsClosedForMismatchMalformedAndUnownedResiduals) {
  const RegistrationState wrong_owner =
      FullState(L"C:\\Other\\ZRinputTsf.dll");
  FakeBackend mismatch(wrong_owner);
  ZR_EXPECT_EQ(UnregisterWithBackend(mismatch, kModulePath),
               HRESULT_FROM_WIN32(ERROR_NOT_OWNER));
  ZR_EXPECT_EQ(mismatch.state, wrong_owner);
  ZR_EXPECT_EQ(mismatch.log, std::vector<Step>{Step::kCapture});

  RegistrationState malformed = FullState();
  malformed.com.default_value.type = REG_BINARY;
  FakeBackend invalid(malformed);
  ZR_EXPECT_EQ(UnregisterWithBackend(invalid, kModulePath),
               HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
  ZR_EXPECT_EQ(invalid.state, malformed);

  RegistrationState residual;
  residual.processor = true;
  FakeBackend no_owner(residual);
  ZR_EXPECT_EQ(UnregisterWithBackend(no_owner, kModulePath),
               HRESULT_FROM_WIN32(ERROR_NOT_OWNER));
  ZR_EXPECT_EQ(no_owner.state, residual);

  RegistrationState parent_only;
  parent_only.com.clsid_key_existed = true;
  FakeBackend already_removed(parent_only);
  ZR_EXPECT_EQ(UnregisterWithBackend(already_removed, kModulePath), S_OK);
  ZR_EXPECT_EQ(already_removed.state, parent_only);
}

ZR_TEST(EveryUnregisterTsfFailureKeepsComBeforeAndAfterSideEffect) {
  const std::array steps = {
      Step::kRemoveCategory0, Step::kRemoveCategory1,
      Step::kRemoveCategory2, Step::kRemoveProfile,
      Step::kRemoveProcessor,
  };
  for (const Step step : steps) {
    for (const bool after_mutation : {false, true}) {
      FakeBackend backend(FullState());
      backend.Fail(step, kPrimaryFailure, after_mutation);
      ZR_EXPECT_EQ(UnregisterWithBackend(backend, kModulePath),
                   kPrimaryFailure);
      ZR_EXPECT_EQ(backend.state.com, DesiredCom());
      ZR_EXPECT_EQ(backend.Calls(Step::kRemoveCom), std::size_t{0});
      for (std::size_t index = 0; index < kRegistrationCategoryCount; ++index) {
        ZR_EXPECT_EQ(backend.Calls(RemoveCategoryStep(index)), std::size_t{1});
      }
    }
  }
}

ZR_TEST(UnregisterReturnsFirstCleanupFailureButAttemptsIndependentCleanup) {
  FakeBackend backend(FullState());
  backend.Fail(Step::kRemoveCategory0, kPrimaryFailure);
  backend.Fail(Step::kRemoveCategory1, kSecondFailure);
  backend.Fail(Step::kRemoveProfile, E_ACCESSDENIED);
  ZR_EXPECT_EQ(UnregisterWithBackend(backend, kModulePath), kPrimaryFailure);
  ZR_EXPECT_EQ(backend.Calls(Step::kRemoveCategory0), std::size_t{1});
  ZR_EXPECT_EQ(backend.Calls(Step::kRemoveCategory1), std::size_t{1});
  ZR_EXPECT_EQ(backend.Calls(Step::kRemoveCategory2), std::size_t{1});
  ZR_EXPECT_EQ(backend.Calls(Step::kRemoveProfile), std::size_t{1});
  ZR_EXPECT_EQ(backend.Calls(Step::kRemoveProcessor), std::size_t{0});
  ZR_EXPECT_EQ(backend.Calls(Step::kRemoveCom), std::size_t{0});
  ZR_EXPECT_EQ(backend.state.com, DesiredCom());
}

ZR_TEST(UnregisterComDeletionFailureIsPropagatedAfterTsfCleanup) {
  FakeBackend backend(FullState());
  backend.Fail(Step::kRemoveCom, E_ACCESSDENIED);
  ZR_EXPECT_EQ(UnregisterWithBackend(backend, kModulePath), E_ACCESSDENIED);
  ZR_EXPECT_TRUE(!backend.state.processor);
  ZR_EXPECT_TRUE(!backend.state.profile);
  ZR_EXPECT_TRUE(std::ranges::none_of(
      backend.state.categories, [](bool value) { return value; }));
  ZR_EXPECT_EQ(backend.state.com, DesiredCom());
}

ZR_TEST(UnregisterPostCleanupProbeFailureKeepsComForRetry) {
  FakeBackend backend(FullState());
  backend.Fail(Step::kCapture, E_ACCESSDENIED, false, 2);
  ZR_EXPECT_EQ(UnregisterWithBackend(backend, kModulePath), E_ACCESSDENIED);
  ZR_EXPECT_TRUE(!backend.state.processor);
  ZR_EXPECT_TRUE(!backend.state.profile);
  ZR_EXPECT_TRUE(std::ranges::none_of(
      backend.state.categories, [](bool value) { return value; }));
  ZR_EXPECT_EQ(backend.state.com, DesiredCom());
  ZR_EXPECT_EQ(backend.Calls(Step::kRemoveCom), std::size_t{0});
}

ZR_TEST(ComDeletionFailureAfterItsSideEffectCanBeRetriedIdempotently) {
  FakeBackend backend(FullState());
  backend.Fail(Step::kRemoveCom, E_ACCESSDENIED, true);
  ZR_EXPECT_EQ(UnregisterWithBackend(backend, kModulePath), E_ACCESSDENIED);
  ZR_EXPECT_EQ(backend.state, RegistrationState{});
  ZR_EXPECT_EQ(UnregisterWithBackend(backend, kModulePath), S_OK);
  ZR_EXPECT_EQ(backend.state, RegistrationState{});
}

ZR_TEST(FailedCleanupAfterItsSideEffectCanBeRetriedIdempotently) {
  FakeBackend backend(FullState());
  backend.Fail(Step::kRemoveProcessor, kPrimaryFailure, true);
  ZR_EXPECT_EQ(UnregisterWithBackend(backend, kModulePath), kPrimaryFailure);
  ZR_EXPECT_EQ(backend.state.com, DesiredCom());
  ZR_EXPECT_TRUE(!backend.state.processor);
  ZR_EXPECT_EQ(UnregisterWithBackend(backend, kModulePath), S_OK);
  ZR_EXPECT_EQ(backend.state, RegistrationState{});
}

ZR_TEST(RegisterAndUnregisterRejectEmptyModulePaths) {
  FakeBackend backend;
  ZR_EXPECT_EQ(RegisterWithBackend(backend, L""), E_INVALIDARG);
  ZR_EXPECT_EQ(UnregisterWithBackend(backend, L""), E_INVALIDARG);

  std::wstring embedded_path{kModulePath};
  embedded_path.insert(3, 1, L'\0');
  ZR_EXPECT_EQ(RegisterWithBackend(backend, embedded_path), E_INVALIDARG);
  ZR_EXPECT_EQ(UnregisterWithBackend(backend, embedded_path), E_INVALIDARG);
  ZR_EXPECT_TRUE(backend.log.empty());
}

}  // namespace

int wmain() { return zrinput::test::RunAll(); }
