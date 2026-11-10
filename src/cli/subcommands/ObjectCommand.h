#pragma once

#include "cli/SubCommand.h"

#include <string>

namespace neurus::cli
{

/// `neurus object`: read and write scene objects.
class ObjectCommand final : public SubCommand
{
public:
	void Register(CLI::App& parent, const GlobalOptions& globals) override;
	bool Selected() const override;
	CliExit Execute() override;

private:
	CliExit ExecuteGet(bool shorthand);
	CliExit ExecuteLoad();
	CliExit ExecuteAdd();
	CliExit ExecuteSet();
	CliExit ExecuteDuplicate();
	CliExit ExecuteDelete();

	CLI::App* m_app = nullptr;
	CLI::App* m_getApp = nullptr;
	CLI::App* m_loadApp = nullptr;
	CLI::App* m_addApp = nullptr;
	CLI::App* m_setApp = nullptr;
	CLI::App* m_duplicateApp = nullptr;
	CLI::App* m_deleteApp = nullptr;

	// Shorthand `object --name ... --position`.
	std::string m_getName;
	int         m_getUid = 0;
	std::string m_getType;
	bool        m_getPosition = false;
	bool        m_getRotation = false;
	bool        m_getScale = false;
	bool        m_getVisible = false;
	std::string m_getSession;
	std::string m_getFormat;
	int         m_getTimeout = 10;

	std::string m_loadPath;
	std::string m_loadName;
	std::string m_loadSession;
	std::string m_loadFormat;
	int         m_loadTimeout = 10;

	std::string m_addType;
	std::string m_addName;
	std::string m_addPosition;
	std::string m_addTarget;
	std::string m_addFov;
	std::string m_addActive;
	std::string m_addSession;
	std::string m_addFormat;
	int         m_addTimeout = 10;

	std::string m_setName;
	int         m_setUid = 0;
	std::string m_setType;
	std::string m_setRename;
	std::string m_setPosition;
	std::string m_setRotation;
	std::string m_setScale;
	std::string m_setVisible;
	std::string m_setViewportVisible;
	std::string m_setRenderVisible;
	std::string m_setActive;
	std::string m_setFov;
	std::string m_setTarget;
	std::string m_setPower;
	std::string m_setColor;
	std::string m_setRadius;
	std::string m_setCutoff;
	std::string m_setOuterCutoff;
	std::string m_setShadow;
	std::string m_setMaterial;
	std::string m_setIntensity;
	std::string m_setEnvironmentRotation;
	std::string m_setSession;
	std::string m_setFormat;
	int         m_setTimeout = 10;

	std::string m_deleteName;
	int         m_deleteUid = 0;
	std::string m_deleteType;
	std::string m_deleteUids;
	std::string m_deleteSession;
	std::string m_deleteFormat;
	int         m_deleteTimeout = 10;

	std::string m_duplicateName;
	int         m_duplicateUid = 0;
	std::string m_duplicateType;
	std::string m_duplicateRename;
	std::string m_duplicateSession;
	std::string m_duplicateFormat;
	int         m_duplicateTimeout = 10;
};

} // namespace neurus::cli
