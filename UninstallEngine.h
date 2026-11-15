#pragma once

#include <vector>
#include <functional>
#include <mutex>
#include <msi.h>

enum class ProgramKind { Msi, Exe, Orphan };

struct ProgramInfo
{
	CString name;
	CString version;
	CString publisher;
	CString installLocation;
	ProgramKind kind = ProgramKind::Exe;

	CString productCode;
	MSIINSTALLCONTEXT context = MSIINSTALLCONTEXT_NONE;
	CString userSid;

	CString uninstallString;
	CString quietUninstallString;

	HKEY arpRoot = nullptr;
	REGSAM arpView = 0;
	CString arpSubKey;

	bool checked = false;
	CString status;
};

class CLogger
{
public:
	~CLogger();
	bool Open(const CString& path);
	void Write(LPCWSTR fmt, ...);
	const CString& Path() const { return m_path; }

private:
	FILE* m_fp = nullptr;
	CString m_path;
	std::mutex m_lock;
};

struct RunContext
{
	CLogger* log = nullptr;
	CString logDir;
	std::function<bool(const CString&)> confirm;   // 返回 true 表示用户同意
	std::function<void(const CString&)> progress;  // 当前程序的状态文字
};

LPCWSTR KindText(ProgramKind kind);
CString CompressGuid(const CString& guid);
std::vector<ProgramInfo> EnumeratePrograms();
CString UninstallOne(const ProgramInfo& p, RunContext& ctx);
