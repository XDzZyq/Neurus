/**
 * @file ScriptEngine.cpp
 * @brief Thin forwarding layer over the PIMPL body in `ScriptEngineImpl.h`.
 */

#include "script/ScriptEngine.h"

#include "script/ScriptEngineImpl.h"

namespace neurus
{

ScriptEngine::ScriptEngine()
	: m_impl(std::make_unique<Impl>())
{
}

ScriptEngine::~ScriptEngine()
{
	// A forgotten Shutdown() must not leave the interpreter running; Shutdown()
	// is guarded and therefore safe to call here.
	if (m_impl)
	{
		m_impl->Shutdown();
	}
}

bool ScriptEngine::Initialize(const std::string& envPath)
{
	return m_impl->Init(envPath);
}

void ScriptEngine::Shutdown()
{
	m_impl->Shutdown();
}

bool ScriptEngine::IsReady() const
{
	return m_impl->IsReady();
}

std::string ScriptEngine::PythonVersion() const
{
	return m_impl->PythonVersion();
}

std::string ScriptEngine::EnvironmentPath() const
{
	return m_impl->EnvironmentPath();
}

std::vector<ScriptPreloadResult> ScriptEngine::Preload(const std::vector<std::string>& packages)
{
	return m_impl->Preload(packages);
}

std::vector<ScriptModuleInfo> ScriptEngine::LoadedModules() const
{
	return m_impl->LoadedModules();
}

void ScriptEngine::SetReloadRoots(std::vector<std::string> roots)
{
	m_impl->SetReloadRoots(roots);
}

std::vector<std::string> ScriptEngine::ReloadRoots() const
{
	return m_impl->ReloadRoots();
}

std::string ScriptEngine::EvalString(const std::string& expression, const ScriptContext* ctx,
                                     const ScriptPolicy* policy)
{
	return m_impl->EvalString(expression, ctx, policy);
}

std::optional<std::string> ScriptEngine::EvalStringChecked(const std::string& expression,
                                                           const ScriptContext* ctx,
                                                           const ScriptPolicy* policy)
{
	return m_impl->EvalStringChecked(expression, ctx, policy);
}

bool ScriptEngine::RunStringChecked(const std::string& code, const ScriptContext* ctx,
                                    const ScriptPolicy* policy)
{
	return m_impl->RunStringChecked(code, ctx, policy);
}

bool ScriptEngine::RunFileChecked(const std::string& path, const ScriptContext* ctx,
                                  const ScriptPolicy* policy)
{
	return m_impl->RunFileChecked(path, ctx, policy);
}

void ScriptEngine::SetEventQueue(IEventQueue* queue)
{
	m_impl->SetEventQueue(queue);
}

void ScriptEngine::RunString(const std::string& code, const ScriptContext* ctx,
                             const ScriptPolicy* policy)
{
	m_impl->RunString(code, ctx, policy);
}

void ScriptEngine::RunFile(const std::string& path, const ScriptContext* ctx,
                           const ScriptPolicy* policy)
{
	m_impl->RunFile(path, ctx, policy);
}

ScriptRunStatus ScriptEngine::LastRunStatus() const
{
	return m_impl->LastRunStatus();
}

std::string ScriptEngine::LastRunMessage() const
{
	return m_impl->LastRunMessage();
}

ScriptPolicy ScriptEngine::LastPolicy() const
{
	return m_impl->LastPolicy();
}

void ScriptEngine::Submit(const std::string& code)
{
	m_impl->Submit(code, nullptr);
}

void ScriptEngine::Submit(const std::string& code, const ScriptPolicy& policy)
{
	m_impl->Submit(code, &policy);
}

void ScriptEngine::SubmitFile(const std::string& path)
{
	m_impl->SubmitFile(path, nullptr);
}

void ScriptEngine::SubmitFile(const std::string& path, const ScriptPolicy& policy)
{
	m_impl->SubmitFile(path, &policy);
}

void ScriptEngine::ExecutePending(const ScriptContext& ctx)
{
	m_impl->ExecutePending(ctx);
}

ScriptConsoleBuffer& ScriptEngine::Console()
{
	return m_impl->Console();
}

}  // namespace neurus
