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

void ScriptEngine::RunString(const std::string& code)
{
	m_impl->RunString(code);
}

}  // namespace neurus
