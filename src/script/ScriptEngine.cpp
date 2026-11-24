/**
 * @file ScriptEngine.cpp
 * @brief Thin forwarding layer over the PIMPL body in `ScriptEngineImpl.hpp`.
 */

#include "script/ScriptEngine.hpp"

#include "script/ScriptEngineImpl.hpp"

namespace neurus
{

ScriptEngine::ScriptEngine()
	: sc_impl(std::make_unique<Impl>())
{
}

ScriptEngine::~ScriptEngine()
{
	// A forgotten Shutdown() must not leave the interpreter running; Shutdown()
	// is guarded and therefore safe to call here.
	if (sc_impl)
	{
		sc_impl->Shutdown();
	}
}

bool ScriptEngine::Initialize(const std::string& envPath)
{
	return sc_impl->Init(envPath);
}

void ScriptEngine::Shutdown()
{
	sc_impl->Shutdown();
}

bool ScriptEngine::IsReady() const
{
	return sc_impl->IsReady();
}

std::string ScriptEngine::PythonVersion() const
{
	return sc_impl->PythonVersion();
}

std::string ScriptEngine::EnvironmentPath() const
{
	return sc_impl->EnvironmentPath();
}

std::vector<ScriptPreloadResult> ScriptEngine::Preload(const std::vector<std::string>& packages)
{
	return sc_impl->Preload(packages);
}

std::vector<ScriptModuleInfo> ScriptEngine::LoadedModules() const
{
	return sc_impl->LoadedModules();
}

void ScriptEngine::SetReloadRoots(std::vector<std::string> roots)
{
	sc_impl->SetReloadRoots(roots);
}

std::vector<std::string> ScriptEngine::ReloadRoots() const
{
	return sc_impl->ReloadRoots();
}

std::string ScriptEngine::EvalString(const std::string& expression, const ScriptContext* ctx,
                                     const ScriptPolicy* policy)
{
	return sc_impl->EvalString(expression, ctx, policy);
}

std::optional<std::string> ScriptEngine::EvalStringChecked(const std::string& expression,
                                                           const ScriptContext* ctx,
                                                           const ScriptPolicy* policy)
{
	return sc_impl->EvalStringChecked(expression, ctx, policy);
}

bool ScriptEngine::RunStringChecked(const std::string& code, const ScriptContext* ctx,
                                    const ScriptPolicy* policy)
{
	return sc_impl->RunStringChecked(code, ctx, policy);
}

bool ScriptEngine::RunFileChecked(const std::string& path, const ScriptContext* ctx,
                                  const ScriptPolicy* policy)
{
	return sc_impl->RunFileChecked(path, ctx, policy);
}

void ScriptEngine::SetEventQueue(IEventQueue* queue)
{
	sc_impl->SetEventQueue(queue);
}

void ScriptEngine::RunString(const std::string& code, const ScriptContext* ctx,
                             const ScriptPolicy* policy)
{
	sc_impl->RunString(code, ctx, policy);
}

void ScriptEngine::RunFile(const std::string& path, const ScriptContext* ctx,
                           const ScriptPolicy* policy)
{
	sc_impl->RunFile(path, ctx, policy);
}

ScriptRunStatus ScriptEngine::LastRunStatus() const
{
	return sc_impl->LastRunStatus();
}

std::string ScriptEngine::LastRunMessage() const
{
	return sc_impl->LastRunMessage();
}

ScriptPolicy ScriptEngine::LastPolicy() const
{
	return sc_impl->LastPolicy();
}

void ScriptEngine::Submit(const std::string& code)
{
	sc_impl->Submit(code, nullptr);
}

void ScriptEngine::Submit(const std::string& code, const ScriptPolicy& policy)
{
	sc_impl->Submit(code, &policy);
}

void ScriptEngine::SubmitFile(const std::string& path)
{
	sc_impl->SubmitFile(path, nullptr);
}

void ScriptEngine::SubmitFile(const std::string& path, const ScriptPolicy& policy)
{
	sc_impl->SubmitFile(path, &policy);
}

void ScriptEngine::ExecutePending(const ScriptContext& ctx)
{
	sc_impl->ExecutePending(ctx);
}

ScriptConsoleBuffer& ScriptEngine::Console()
{
	return sc_impl->Console();
}

}  // namespace neurus
