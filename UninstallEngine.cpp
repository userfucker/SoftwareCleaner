#include "pch.h"
#include "UninstallEngine.h"

#include <set>
#include <map>
#include <algorithm>
#include <sddl.h>

#pragma comment(lib, "msi.lib")

namespace
{
const wchar_t kArpPath[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

const DWORD kMsiLogMode =
	INSTALLLOGMODE_FATALEXIT | INSTALLLOGMODE_ERROR | INSTALLLOGMODE_WARNING |
	INSTALLLOGMODE_USER | INSTALLLOGMODE_INFO | INSTALLLOGMODE_RESOLVESOURCE |
	INSTALLLOGMODE_OUTOFDISKSPACE | INSTALLLOGMODE_ACTIONSTART | INSTALLLOGMODE_ACTIONDATA |
	INSTALLLOGMODE_COMMONDATA | INSTALLLOGMODE_PROPERTYDUMP | INSTALLLOGMODE_VERBOSE;

// ---------------------------------------------------------------- 注册表工具

// 组件注册里的路径可能带 '?' 标记（如 C:\?Windows\system32\x.dll、C?\Program Files\...），还原成真实路径
CString NormalizeKeyPath(const CString& raw)
{
	CString s(raw);
	if (s.GetLength() > 2 && iswalpha(s[0]) && s[1] == L'?')
		s.SetAt(1, L':');
	s.Remove(L'?');
	return s;
}

// 组件 KeyPath 是本地文件（不是注册表路径、不是目录）
bool IsFileKeyPath(const CString& s)
{
	return s.GetLength() > 3 && iswalpha(s[0]) && s[1] == L':' && s[2] == L'\\' && s.Right(1) != L"\\";
}

CString KeyName(HKEY root, const CString& sub, REGSAM view)
{
	CString r = (root == HKEY_LOCAL_MACHINE) ? L"HKLM" : (root == HKEY_CURRENT_USER) ? L"HKCU" : L"HK?";
	if (view == KEY_WOW64_32KEY)
		r += L"(32位)";
	return r + L"\\" + sub;
}

CString RegReadString(HKEY h, LPCWSTR name)
{
	DWORD type = 0, cb = 0;
	if (RegQueryValueExW(h, name, nullptr, &type, nullptr, &cb) != ERROR_SUCCESS)
		return CString();
	if (type != REG_SZ && type != REG_EXPAND_SZ)
		return CString();

	CString s;
	DWORD cch = cb / sizeof(WCHAR);
	LPWSTR buf = s.GetBuffer(cch + 1);
	if (RegQueryValueExW(h, name, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &cb) != ERROR_SUCCESS)
		cb = 0;
	buf[cb / sizeof(WCHAR)] = L'\0';
	s.ReleaseBuffer();
	return s;
}

DWORD RegReadDword(HKEY h, LPCWSTR name)
{
	DWORD v = 0, cb = sizeof(v);
	if (RegGetValueW(h, nullptr, name, RRF_RT_REG_DWORD, nullptr, &v, &cb) != ERROR_SUCCESS)
		return 0;
	return v;
}

std::vector<CString> RegSubKeys(HKEY root, const CString& sub, REGSAM view)
{
	std::vector<CString> out;
	HKEY h = nullptr;
	if (RegOpenKeyExW(root, sub, 0, KEY_READ | view, &h) != ERROR_SUCCESS)
		return out;

	WCHAR name[256];
	for (DWORD i = 0;; ++i)
	{
		DWORD cch = _countof(name);
		LONG r = RegEnumKeyExW(h, i, name, &cch, nullptr, nullptr, nullptr, nullptr);
		if (r == ERROR_NO_MORE_ITEMS)
			break;
		if (r == ERROR_SUCCESS)
			out.emplace_back(name);
	}
	RegCloseKey(h);
	return out;
}

bool RegKeyExists(HKEY root, const CString& sub, REGSAM view)
{
	HKEY h = nullptr;
	if (RegOpenKeyExW(root, sub, 0, KEY_READ | view, &h) != ERROR_SUCCESS)
		return false;
	RegCloseKey(h);
	return true;
}

bool RegDeleteKeyTree(HKEY root, const CString& path, REGSAM view, CLogger& log)
{
	int pos = path.ReverseFind(L'\\');
	if (pos <= 0)
		return false;

	HKEY parent = nullptr;
	if (RegOpenKeyExW(root, path.Left(pos), 0, KEY_ALL_ACCESS | view, &parent) != ERROR_SUCCESS)
		return false;

	LONG r = RegDeleteTreeW(parent, path.Mid(pos + 1));
	RegCloseKey(parent);

	if (r == ERROR_SUCCESS)
		log.Write(L"  删除注册表键 %s", KeyName(root, path, view).GetString());
	else if (r != ERROR_FILE_NOT_FOUND)
		log.Write(L"  删除注册表键失败（错误 %ld）%s", r, KeyName(root, path, view).GetString());
	return r == ERROR_SUCCESS;
}

// 删除 compRoot 下每个组件里名为 compressed 的值；组件只属于这个产品时，记录它的文件 KeyPath 并删掉整个组件键
void RemoveComponentClients(const CString& compRoot, const CString& compressed,
	std::vector<CString>& files, CLogger& log)
{
	HKEY hRoot = nullptr;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, compRoot, 0, KEY_READ | KEY_WOW64_64KEY, &hRoot) != ERROR_SUCCESS)
		return;

	std::vector<CString> emptyKeys;
	WCHAR name[256];
	for (DWORD i = 0;; ++i)
	{
		DWORD cch = _countof(name);
		LONG r = RegEnumKeyExW(hRoot, i, name, &cch, nullptr, nullptr, nullptr, nullptr);
		if (r == ERROR_NO_MORE_ITEMS)
			break;
		if (r != ERROR_SUCCESS)
			continue;

		HKEY h = nullptr;
		if (RegOpenKeyExW(hRoot, name, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &h) != ERROR_SUCCESS)
			continue;

		if (RegQueryValueExW(h, compressed, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
		{
			DWORD valueCount = 0;
			RegQueryInfoKeyW(h, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
				&valueCount, nullptr, nullptr, nullptr, nullptr);

			CString keyPath = NormalizeKeyPath(RegReadString(h, compressed));
			bool exclusive = valueCount <= 1;

			if (exclusive && IsFileKeyPath(keyPath))
				files.push_back(keyPath);
			if (RegDeleteValueW(h, compressed) == ERROR_SUCCESS && !exclusive)
				log.Write(L"  组件 %s 被其他产品共享，只移除本产品的引用", name);
			if (exclusive)
				emptyKeys.emplace_back(name);
		}
		RegCloseKey(h);
	}
	RegCloseKey(hRoot);

	for (const CString& k : emptyKeys)
		RegDeleteKeyTree(HKEY_LOCAL_MACHINE, compRoot + L"\\" + k, KEY_WOW64_64KEY, log);
}

// UpgradeCodes\<压缩升级码> 下名为 compressed 的值
void RemoveUpgradeCodeValues(HKEY root, const CString& upgradeRoot, REGSAM view,
	const CString& compressed, CLogger& log)
{
	HKEY hRoot = nullptr;
	if (RegOpenKeyExW(root, upgradeRoot, 0, KEY_READ | view, &hRoot) != ERROR_SUCCESS)
		return;

	std::vector<CString> emptyKeys;
	WCHAR name[256];
	for (DWORD i = 0;; ++i)
	{
		DWORD cch = _countof(name);
		LONG r = RegEnumKeyExW(hRoot, i, name, &cch, nullptr, nullptr, nullptr, nullptr);
		if (r == ERROR_NO_MORE_ITEMS)
			break;
		if (r != ERROR_SUCCESS)
			continue;

		HKEY h = nullptr;
		if (RegOpenKeyExW(hRoot, name, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &h) != ERROR_SUCCESS)
			continue;

		if (RegDeleteValueW(h, compressed) == ERROR_SUCCESS)
		{
			log.Write(L"  删除升级码引用 %s\\%s", KeyName(root, upgradeRoot, view).GetString(), name);
			DWORD values = 0, subKeys = 0;
			RegQueryInfoKeyW(h, nullptr, nullptr, nullptr, &subKeys, nullptr, nullptr,
				&values, nullptr, nullptr, nullptr, nullptr);
			if (values == 0 && subKeys == 0)
				emptyKeys.emplace_back(name);
		}
		RegCloseKey(h);
	}
	RegCloseKey(hRoot);

	for (const CString& k : emptyKeys)
		RegDeleteKeyTree(root, upgradeRoot + L"\\" + k, view, log);
}

void DeleteArpKeys(const CString& productCode, CLogger& log)
{
	CString path = CString(kArpPath) + L"\\" + productCode;
	RegDeleteKeyTree(HKEY_LOCAL_MACHINE, path, KEY_WOW64_64KEY, log);
	RegDeleteKeyTree(HKEY_LOCAL_MACHINE, path, KEY_WOW64_32KEY, log);
	RegDeleteKeyTree(HKEY_CURRENT_USER, path, 0, log);
}

// ---------------------------------------------------------------- 文件工具

bool IsProtectedPath(const CString& path)
{
	WCHAR win[MAX_PATH] = {};
	GetWindowsDirectoryW(win, MAX_PATH);
	CString w(win);
	w += L"\\";
	return path.GetLength() < 4 || _wcsnicmp(path, w, w.GetLength()) == 0;
}

void DeleteFileSafe(const CString& path, CLogger& log)
{
	if (IsProtectedPath(path))
	{
		log.Write(L"  跳过系统目录里的文件 %s", path.GetString());
		return;
	}
	DWORD attr = GetFileAttributesW(path);
	if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY))
		return;

	SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
	if (DeleteFileW(path))
		log.Write(L"  删除文件 %s", path.GetString());
	else if (MoveFileExW(path, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT))
		log.Write(L"  文件被占用，重启后删除 %s", path.GetString());
	else
		log.Write(L"  删除文件失败（错误 %lu）%s", GetLastError(), path.GetString());
}

void RemoveEmptyParents(const std::vector<CString>& files, CLogger& log)
{
	for (const CString& f : files)
	{
		CString dir = f.Left(f.ReverseFind(L'\\'));
		while (!IsProtectedPath(dir))
		{
			int depth = 0;
			for (int i = 0; i < dir.GetLength(); ++i)
				if (dir[i] == L'\\')
					++depth;
			if (depth < 2)
				break;   // 不碰 C:\xxx 这一层
			if (!RemoveDirectoryW(dir))
				break;   // 目录不空或不存在
			log.Write(L"  删除空目录 %s", dir.GetString());
			dir = dir.Left(dir.ReverseFind(L'\\'));
		}
	}
}

// ---------------------------------------------------------------- 其他工具

bool IsGuidString(const CString& s)
{
	return s.GetLength() == 38 && s[0] == L'{' && s[37] == L'}' &&
		s[9] == L'-' && s[14] == L'-' && s[19] == L'-' && s[24] == L'-';
}

CString ExtractGuid(const CString& text)
{
	int pos = text.Find(L'{');
	if (pos < 0 || pos + 38 > text.GetLength())
		return CString();
	CString g = text.Mid(pos, 38);
	return IsGuidString(g) ? g : CString();
}

CString ExpandEnv(const CString& s)
{
	DWORD n = ExpandEnvironmentStringsW(s, nullptr, 0);
	if (n == 0)
		return s;
	CString out;
	ExpandEnvironmentStringsW(s, out.GetBuffer(n), n);
	out.ReleaseBuffer();
	return out;
}

CString ErrorText(DWORD code)
{
	LPWSTR buf = nullptr;
	FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
	CString s(buf ? buf : L"未知错误");
	if (buf)
		LocalFree(buf);
	s.Trim();
	return s;
}

// "Microsoft Corporation"、"Microsoft Corp." 等统一显示为 Microsoft
CString NormalizePublisher(const CString& publisher)
{
	CString t(publisher);
	t.Trim();
	if (_wcsnicmp(t, L"Microsoft", 9) == 0 && (t.GetLength() == 9 || !iswalpha(t[9])))
		return L"Microsoft";
	return t;
}

CString CleanDir(const CString& dir)
{
	CString d(dir);
	d.Trim();
	d.Trim(L'"');
	while (d.GetLength() > 3 && d.Right(1) == L"\\")
		d.Truncate(d.GetLength() - 1);
	return d;
}

// 从命令行或 DisplayIcon 里取出 exe 路径："C:\a b\x.exe" /S、C:\a\x.exe,0 这几种写法
CString ExePathFromCommand(const CString& cmd)
{
	CString s(cmd);
	s.Trim();
	if (s.IsEmpty())
		return s;
	if (s[0] == L'"')
	{
		int end = s.Find(L'"', 1);
		return end > 1 ? s.Mid(1, end - 1) : CString();
	}
	CString lower(s);
	lower.MakeLower();
	int pos = lower.Find(L".exe");
	if (pos >= 0)
		return s.Left(pos + 4);
	int comma = s.Find(L',');
	return comma > 0 ? s.Left(comma) : s;
}

// 由卸载程序或图标的路径推断安装目录，排除系统目录和安装包缓存目录
CString DirFromCommand(const CString& cmd)
{
	CString path = ExpandEnv(ExePathFromCommand(cmd));
	if (path.GetLength() < 4 || path[1] != L':' || IsProtectedPath(path))
		return CString();

	CString lower(path);
	lower.MakeLower();
	if (lower.Find(L"\\package cache\\") >= 0 || lower.Find(L"\\installshield installation information\\") >= 0 ||
		lower.Find(L"\\installer\\") >= 0)
		return CString();

	int pos = path.ReverseFind(L'\\');
	if (pos <= 2)
		return CString();
	CString dir = path.Left(pos);

	CString leaf = dir.Mid(dir.ReverseFind(L'\\') + 1);
	leaf.MakeLower();
	if (leaf.Find(L"uninst") == 0)
	{
		int up = dir.ReverseFind(L'\\');
		if (up > 2)
			dir = dir.Left(up);
	}
	return dir;
}

CString LocationFromArpKey(HKEY h)
{
	CString loc = CleanDir(ExpandEnv(RegReadString(h, L"InstallLocation")));
	if (loc.IsEmpty())
		loc = DirFromCommand(RegReadString(h, L"DisplayIcon"));
	if (loc.IsEmpty())
		loc = DirFromCommand(RegReadString(h, L"UninstallString"));
	return loc;
}

CString MsiArpLocation(const CString& productCode)
{
	CString path = CString(kArpPath) + L"\\" + productCode;
	struct Source { HKEY root; REGSAM view; };
	const Source sources[] = {
		{ HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY },
		{ HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY },
		{ HKEY_CURRENT_USER, 0 },
	};
	for (const Source& src : sources)
	{
		HKEY h = nullptr;
		if (RegOpenKeyExW(src.root, path, 0, KEY_READ | src.view, &h) != ERROR_SUCCESS)
			continue;
		CString loc = LocationFromArpKey(h);
		RegCloseKey(h);
		if (!loc.IsEmpty())
			return loc;
	}
	return CString();
}

// 压缩产品码 → 这个产品所有组件的文件路径（一次遍历 UserData\*\Components）
using ComponentFileMap = std::map<CString, std::vector<CString>>;

ComponentFileMap BuildComponentFileMap()
{
	ComponentFileMap map;
	const CString userData = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Installer\\UserData";
	for (const CString& sid : RegSubKeys(HKEY_LOCAL_MACHINE, userData, KEY_WOW64_64KEY))
	{
		HKEY hRoot = nullptr;
		if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, userData + L"\\" + sid + L"\\Components", 0,
			KEY_READ | KEY_WOW64_64KEY, &hRoot) != ERROR_SUCCESS)
			continue;

		WCHAR comp[256];
		for (DWORD i = 0;; ++i)
		{
			DWORD cchComp = _countof(comp);
			LONG r = RegEnumKeyExW(hRoot, i, comp, &cchComp, nullptr, nullptr, nullptr, nullptr);
			if (r == ERROR_NO_MORE_ITEMS)
				break;
			if (r != ERROR_SUCCESS)
				continue;

			HKEY h = nullptr;
			if (RegOpenKeyExW(hRoot, comp, 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS)
				continue;

			WCHAR name[64];
			WCHAR data[MAX_PATH * 2];
			for (DWORD v = 0;; ++v)
			{
				DWORD cchName = _countof(name);
				DWORD cbData = sizeof(data) - sizeof(WCHAR);
				DWORD type = 0;
				LONG rv = RegEnumValueW(h, v, name, &cchName, nullptr, &type, reinterpret_cast<LPBYTE>(data), &cbData);
				if (rv == ERROR_NO_MORE_ITEMS)
					break;
				if (rv != ERROR_SUCCESS || type != REG_SZ)
					continue;
				data[cbData / sizeof(WCHAR)] = L'\0';
				CString path = NormalizeKeyPath(data);
				if (IsFileKeyPath(path))
					map[CString(name)].push_back(path);
			}
			RegCloseKey(h);
		}
		RegCloseKey(hRoot);
	}
	return map;
}

// 两个目录按整段比较的公共前缀，例如 C:\A\B 和 C:\A\C → C:\A
CString CommonDirPrefix(const CString& a, const CString& b)
{
	int n = min(a.GetLength(), b.GetLength());
	int lastSep = -1;
	int i = 0;
	for (; i < n; ++i)
	{
		if (towlower(a[i]) != towlower(b[i]))
			break;
		if (a[i] == L'\\')
			lastSep = i;
	}
	if (i == n)
	{
		const CString& longer = a.GetLength() > n ? a : b;
		if (longer.GetLength() == n || longer[n] == L'\\')
			return a.Left(n);
	}
	return lastSep > 0 ? a.Left(lastSep) : CString();
}

// Program Files、ProgramData、用户目录这类公共目录不能当作某个程序的安装位置
bool IsGenericRoot(const CString& dir)
{
	if (dir.GetLength() <= 3)
		return true;
	static const wchar_t* roots[] = {
		L"%ProgramFiles%", L"%ProgramFiles(x86)%", L"%ProgramW6432%", L"%CommonProgramFiles%",
		L"%CommonProgramFiles(x86)%", L"%ProgramData%", L"%SystemDrive%\\Users", L"%USERPROFILE%",
		L"%LOCALAPPDATA%", L"%APPDATA%",
	};
	for (const wchar_t* r : roots)
	{
		CString expanded = ExpandEnv(r);
		if (expanded.Find(L'%') < 0 && dir.CompareNoCase(expanded) == 0)
			return true;
	}
	return false;
}

CString GuessInstallDir(const std::vector<CString>& files)
{
	std::vector<CString> dirs;
	for (const CString& f : files)
	{
		if (IsProtectedPath(f))
			continue;
		CString lower(f);
		lower.MakeLower();
		if (lower.Find(L"\\common files\\") >= 0)
			continue;   // 共享组件目录不代表安装位置
		dirs.push_back(f.Left(f.ReverseFind(L'\\')));
	}
	if (dirs.empty())
		return CString();

	CString prefix = dirs[0];
	for (const CString& d : dirs)
		prefix = CommonDirPrefix(prefix, d);
	if (!IsGenericRoot(prefix))
		return prefix;

	// 文件分散在多个地方时，取文件最多的那个目录
	std::map<CString, std::pair<CString, int>> count;
	for (const CString& d : dirs)
	{
		CString key(d);
		key.MakeUpper();
		auto& entry = count[key];
		entry.first = d;
		++entry.second;
	}
	const std::pair<CString, int>* best = nullptr;
	for (const auto& kv : count)
		if (!best || kv.second.second > best->second)
			best = &kv.second;
	return best ? best->first : CString();
}

int KindRank(ProgramKind kind)
{
	switch (kind)
	{
	case ProgramKind::Msi: return 0;
	case ProgramKind::Exe: return 1;
	default:               return 2;
	}
}

CString CurrentUserSid()
{
	CString result;
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
		return result;

	DWORD_PTR buf[64] = {};
	DWORD len = 0;
	if (GetTokenInformation(token, TokenUser, buf, sizeof(buf), &len))
	{
		LPWSTR s = nullptr;
		if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf)->User.Sid, &s))
		{
			result = s;
			LocalFree(s);
		}
	}
	CloseHandle(token);
	return result;
}

CString MsiProp(LPCWSTR code, LPCWSTR sid, MSIINSTALLCONTEXT ctx, LPCWSTR prop)
{
	DWORD cch = 0;
	if (MsiGetProductInfoExW(code, sid, ctx, prop, nullptr, &cch) != ERROR_SUCCESS)
		return CString();
	CString v;
	++cch;
	UINT r = MsiGetProductInfoExW(code, sid, ctx, prop, v.GetBuffer(cch), &cch);
	v.ReleaseBuffer(r == ERROR_SUCCESS ? static_cast<int>(cch) : 0);
	return v;
}

struct RunResult
{
	bool started = false;
	DWORD exitCode = 0;
	DWORD error = 0;
};

// 把卸载程序放进作业对象，连它再启动的子进程（NSIS、Inno 常这样做）一起等完
RunResult RunAndWait(const CString& cmdLine)
{
	RunResult rr;
	CString cmd(cmdLine);
	STARTUPINFOW si = { sizeof(si) };
	PROCESS_INFORMATION pi = {};
	HANDLE job = CreateJobObjectW(nullptr, nullptr);

	BOOL ok = CreateProcessW(nullptr, cmd.GetBuffer(), nullptr, nullptr, FALSE,
		CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
	cmd.ReleaseBuffer();
	if (!ok)
	{
		rr.error = GetLastError();
		if (job)
			CloseHandle(job);
		return rr;
	}

	rr.started = true;
	if (job)
		AssignProcessToJobObject(job, pi.hProcess);
	ResumeThread(pi.hThread);
	WaitForSingleObject(pi.hProcess, INFINITE);
	GetExitCodeProcess(pi.hProcess, &rr.exitCode);

	if (job)
	{
		for (;;)
		{
			JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info = {};
			if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &info, sizeof(info), nullptr) ||
				info.ActiveProcesses == 0)
				break;
			Sleep(500);
		}
		CloseHandle(job);
	}
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return rr;
}

// ---------------------------------------------------------------- 卸载步骤

UINT MsiUninstall(const ProgramInfo& p, RunContext& ctx)
{
	CLogger& log = *ctx.log;
	CString msiLog;
	msiLog.Format(L"%s\\MSI_%s.log", ctx.logDir.GetString(), p.productCode.GetString());

	MsiSetInternalUI(INSTALLUILEVEL_NONE, nullptr);

	UINT rc = ERROR_SUCCESS;
	for (int attempt = 0; attempt < 6; ++attempt)
	{
		MsiEnableLogW(kMsiLogMode, msiLog, INSTALLLOGATTRIBUTES_APPEND | INSTALLLOGATTRIBUTES_FLUSHEACHLINE);
		rc = MsiConfigureProductExW(p.productCode, INSTALLLEVEL_DEFAULT, INSTALLSTATE_ABSENT, L"REBOOT=ReallySuppress");
		MsiEnableLogW(0, nullptr, 0);

		log.Write(L"  MsiConfigureProductEx 返回 %u（%s）", rc, ErrorText(rc).GetString());
		if (rc != ERROR_INSTALL_ALREADY_RUNNING)
			break;
		ctx.progress(L"另一个安装正在进行，10 秒后重试…");
		Sleep(10000);
	}
	log.Write(L"  MSI 详细日志：%s", msiLog.GetString());
	return rc;
}

void ForceRemoveMsi(const CString& productCode, CLogger& log)
{
	CString c = CompressGuid(productCode);
	if (c.IsEmpty())
	{
		log.Write(L"  产品代码格式不对，跳过注册信息清理：%s", productCode.GetString());
		return;
	}
	log.Write(L"  强制删除，压缩 GUID：%s", c.GetString());

	std::vector<CString> files;
	const CString inst = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Installer";

	for (const CString& sid : RegSubKeys(HKEY_LOCAL_MACHINE, inst + L"\\UserData", KEY_WOW64_64KEY))
	{
		CString base = inst + L"\\UserData\\" + sid;
		RemoveComponentClients(base + L"\\Components", c, files, log);
		RegDeleteKeyTree(HKEY_LOCAL_MACHINE, base + L"\\Products\\" + c, KEY_WOW64_64KEY, log);
	}

	for (const CString& sid : RegSubKeys(HKEY_LOCAL_MACHINE, inst + L"\\Managed", KEY_WOW64_64KEY))
	{
		CString base = inst + L"\\Managed\\" + sid + L"\\Installer";
		RegDeleteKeyTree(HKEY_LOCAL_MACHINE, base + L"\\Products\\" + c, KEY_WOW64_64KEY, log);
		RegDeleteKeyTree(HKEY_LOCAL_MACHINE, base + L"\\Features\\" + c, KEY_WOW64_64KEY, log);
		RemoveUpgradeCodeValues(HKEY_LOCAL_MACHINE, base + L"\\UpgradeCodes", KEY_WOW64_64KEY, c, log);
	}

	const CString classes = L"SOFTWARE\\Classes\\Installer";
	RegDeleteKeyTree(HKEY_LOCAL_MACHINE, classes + L"\\Products\\" + c, KEY_WOW64_64KEY, log);
	RegDeleteKeyTree(HKEY_LOCAL_MACHINE, classes + L"\\Features\\" + c, KEY_WOW64_64KEY, log);
	RemoveUpgradeCodeValues(HKEY_LOCAL_MACHINE, classes + L"\\UpgradeCodes", KEY_WOW64_64KEY, c, log);

	const CString user = L"Software\\Microsoft\\Installer";
	RegDeleteKeyTree(HKEY_CURRENT_USER, user + L"\\Products\\" + c, 0, log);
	RegDeleteKeyTree(HKEY_CURRENT_USER, user + L"\\Features\\" + c, 0, log);
	RemoveUpgradeCodeValues(HKEY_CURRENT_USER, user + L"\\UpgradeCodes", 0, c, log);

	DeleteArpKeys(productCode, log);

	for (const CString& f : files)
		DeleteFileSafe(f, log);
	RemoveEmptyParents(files, log);
}

CString UninstallMsi(const ProgramInfo& p, RunContext& ctx)
{
	CLogger& log = *ctx.log;
	bool otherUser = p.context != MSIINSTALLCONTEXT_MACHINE && p.userSid.CompareNoCase(CurrentUserSid()) != 0;

	UINT rc = ERROR_UNKNOWN_PRODUCT;
	if (otherUser)
		log.Write(L"  这是其他用户（%s）的安装，无法用正常方式卸载", p.userSid.GetString());
	else
		rc = MsiUninstall(p, ctx);

	if (rc == ERROR_SUCCESS || rc == ERROR_SUCCESS_REBOOT_REQUIRED || rc == ERROR_SUCCESS_REBOOT_INITIATED)
	{
		DeleteArpKeys(p.productCode, log);
		return rc == ERROR_SUCCESS ? L"卸载成功" : L"卸载成功，需要重启";
	}
	if (rc == ERROR_UNKNOWN_PRODUCT && !otherUser)
	{
		DeleteArpKeys(p.productCode, log);
		return L"产品已不存在，已清理残留条目";
	}
	if (rc == ERROR_INSTALL_USEREXIT)
		return L"已取消";

	CString reason;
	if (otherUser)
		reason = L"它是其他用户安装的";
	else
		reason.Format(L"错误 %u：%s", rc, ErrorText(rc).GetString());

	CString q;
	q.Format(L"“%s”无法正常卸载（%s）。\n\n是否强制删除它的安装注册信息和独占文件？\n此操作不可恢复。",
		p.name.GetString(), reason.GetString());
	if (!ctx.confirm(q))
	{
		log.Write(L"  用户选择跳过强制删除");
		CString s;
		s.Format(L"卸载失败（%u），已跳过", rc);
		return s;
	}
	ForceRemoveMsi(p.productCode, log);
	return L"已强制删除";
}

CString UninstallExe(const ProgramInfo& p, RunContext& ctx)
{
	CLogger& log = *ctx.log;
	CString cmd = ExpandEnv(p.quietUninstallString.IsEmpty() ? p.uninstallString : p.quietUninstallString);
	log.Write(L"  执行：%s", cmd.GetString());
	ctx.progress(L"正在运行卸载程序…");

	RunResult rr = RunAndWait(cmd);
	if (!rr.started)
		log.Write(L"  无法启动卸载程序（错误 %lu：%s）", rr.error, ErrorText(rr.error).GetString());
	else
		log.Write(L"  卸载程序退出码 %lu", rr.exitCode);

	if (rr.started && !RegKeyExists(p.arpRoot, p.arpSubKey, p.arpView))
	{
		if (rr.exitCode == 0)
			return L"卸载成功";
		CString s;
		s.Format(L"卸载完成（退出码 %lu）", rr.exitCode);
		return s;
	}

	CString q;
	q.Format(L"“%s”的卸载程序%s，但卸载条目还在。\n\n可能是卸载被取消或失败了。是否直接删除这个卸载条目？\n（程序文件不会被删除）",
		p.name.GetString(), rr.started ? L"已经结束" : L"无法启动");
	if (!ctx.confirm(q))
	{
		log.Write(L"  用户选择保留卸载条目");
		return L"卸载条目仍在，已跳过";
	}
	RegDeleteKeyTree(p.arpRoot, p.arpSubKey, p.arpView, log);
	return L"已删除卸载条目";
}

CString RemoveOrphan(const ProgramInfo& p, RunContext& ctx)
{
	CLogger& log = *ctx.log;
	CString q;
	q.Format(L"“%s”只剩下控制面板里的卸载条目，程序本身已无法正常卸载。\n\n是否删除这个条目%s？\n此操作不可恢复。",
		p.name.GetString(), p.productCode.IsEmpty() ? L"" : L"以及它残留的 MSI 注册信息");
	if (!ctx.confirm(q))
	{
		log.Write(L"  用户选择跳过");
		return L"已跳过";
	}
	if (!p.productCode.IsEmpty())
		ForceRemoveMsi(p.productCode, log);
	RegDeleteKeyTree(p.arpRoot, p.arpSubKey, p.arpView, log);
	return L"已删除残留条目";
}
} // namespace

// ---------------------------------------------------------------- CLogger

CLogger::~CLogger()
{
	if (m_fp)
		fclose(m_fp);
}

bool CLogger::Open(const CString& path)
{
	m_path = path;
	return _wfopen_s(&m_fp, path, L"a, ccs=UTF-8") == 0;
}

void CLogger::Write(LPCWSTR fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	CString msg;
	msg.FormatV(fmt, args);
	va_end(args);

	std::lock_guard<std::mutex> guard(m_lock);
	if (!m_fp)
		return;
	SYSTEMTIME st;
	GetLocalTime(&st);
	fwprintf(m_fp, L"%02d:%02d:%02d  %s\n", st.wHour, st.wMinute, st.wSecond, msg.GetString());
	fflush(m_fp);
}

// ---------------------------------------------------------------- 对外接口

LPCWSTR KindText(ProgramKind kind)
{
	switch (kind)
	{
	case ProgramKind::Msi: return L"MSI";
	case ProgramKind::Exe: return L"EXE";
	default:               return L"残留条目";
	}
}

// 前三段整段反转，后 8 个字节每字节内两个字符互换
CString CompressGuid(const CString& guid)
{
	if (!IsGuidString(guid))
		return CString();
	static const int seg[][2] = { {1,8},{10,4},{15,4},{20,2},{22,2},{25,2},{27,2},{29,2},{31,2},{33,2},{35,2} };
	CString out;
	for (const auto& s : seg)
	{
		CString part = guid.Mid(s[0], s[1]);
		part.MakeReverse();
		out += part;
	}
	out.MakeUpper();
	return out;
}

std::vector<ProgramInfo> EnumeratePrograms()
{
	std::vector<ProgramInfo> out;
	std::set<CString> msiCodes;

	WCHAR code[39] = {};
	WCHAR sid[256] = {};
	for (DWORD i = 0;; ++i)
	{
		MSIINSTALLCONTEXT ctx = MSIINSTALLCONTEXT_NONE;
		DWORD cchSid = _countof(sid);
		if (MsiEnumProductsExW(nullptr, L"s-1-1-0", MSIINSTALLCONTEXT_ALL, i, code, &ctx, sid, &cchSid) != ERROR_SUCCESS)
			break;

		ProgramInfo p;
		p.kind = ProgramKind::Msi;
		p.productCode = code;
		p.context = ctx;
		if (ctx != MSIINSTALLCONTEXT_MACHINE)
			p.userSid = sid;

		LPCWSTR s = p.userSid.IsEmpty() ? nullptr : p.userSid.GetString();
		p.name = MsiProp(code, s, ctx, INSTALLPROPERTY_PRODUCTNAME);
		if (p.name.IsEmpty())
			p.name = L"（无名称）";
		p.version = MsiProp(code, s, ctx, INSTALLPROPERTY_VERSIONSTRING);
		p.publisher = NormalizePublisher(MsiProp(code, s, ctx, INSTALLPROPERTY_PUBLISHER));
		p.installLocation = CleanDir(MsiProp(code, s, ctx, INSTALLPROPERTY_INSTALLLOCATION));
		if (p.installLocation.IsEmpty())
			p.installLocation = MsiArpLocation(p.productCode);

		CString upper(code);
		upper.MakeUpper();
		msiCodes.insert(upper);
		out.push_back(p);
	}

	struct Source { HKEY root; REGSAM view; };
	const Source sources[] = {
		{ HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY },
		{ HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY },
		{ HKEY_CURRENT_USER, 0 },
	};

	std::set<CString> seen;
	for (const Source& src : sources)
	{
		for (const CString& sub : RegSubKeys(src.root, kArpPath, src.view))
		{
			CString path = CString(kArpPath) + L"\\" + sub;
			HKEY h = nullptr;
			if (RegOpenKeyExW(src.root, path, 0, KEY_READ | src.view, &h) != ERROR_SUCCESS)
				continue;

			ProgramInfo p;
			p.name = RegReadString(h, L"DisplayName");
			p.version = RegReadString(h, L"DisplayVersion");
			p.publisher = NormalizePublisher(RegReadString(h, L"Publisher"));
			p.installLocation = LocationFromArpKey(h);
			p.uninstallString = RegReadString(h, L"UninstallString");
			p.quietUninstallString = RegReadString(h, L"QuietUninstallString");
			DWORD systemComponent = RegReadDword(h, L"SystemComponent");
			DWORD windowsInstaller = RegReadDword(h, L"WindowsInstaller");
			CString parentKey = RegReadString(h, L"ParentKeyName");
			RegCloseKey(h);

			if (p.name.IsEmpty() || !parentKey.IsEmpty())
				continue;   // 没有名字的条目和更新补丁不显示

			CString lowerCmd(p.uninstallString);
			lowerCmd.MakeLower();
			bool msiStyle = windowsInstaller == 1 || lowerCmd.Find(L"msiexec") >= 0;

			if (msiStyle)
			{
				CString g = IsGuidString(sub) ? sub : ExtractGuid(p.uninstallString);
				CString upperG(g);
				upperG.MakeUpper();
				if (!g.IsEmpty() && msiCodes.count(upperG))
					continue;   // 已经作为 MSI 产品列出
				p.kind = ProgramKind::Orphan;   // 注册信息已丢失
				p.productCode = g;
			}
			else
			{
				if (systemComponent == 1)
					continue;
				p.kind = (p.uninstallString.IsEmpty() && p.quietUninstallString.IsEmpty())
					? ProgramKind::Orphan : ProgramKind::Exe;
			}

			// 32 位系统上两个视图是同一个键，去重
			CString dedupe;
			dedupe.Format(L"%p|%s|%s|%s", src.root, sub.GetString(), p.name.GetString(), p.uninstallString.GetString());
			dedupe.MakeUpper();
			if (!seen.insert(dedupe).second)
				continue;

			p.arpRoot = src.root;
			p.arpView = src.view;
			p.arpSubKey = path;
			out.push_back(p);
		}
	}

	// MSI 没登记安装位置的，用它组件的文件路径推断
	bool needGuess = std::any_of(out.begin(), out.end(), [](const ProgramInfo& p) {
		return p.installLocation.IsEmpty() && !p.productCode.IsEmpty();
	});
	if (needGuess)
	{
		ComponentFileMap files = BuildComponentFileMap();
		for (ProgramInfo& p : out)
		{
			if (!p.installLocation.IsEmpty() || p.productCode.IsEmpty())
				continue;
			auto it = files.find(CompressGuid(p.productCode));
			if (it != files.end())
				p.installLocation = GuessInstallDir(it->second);
		}
	}

	// 没有安装位置的不显示；残留条目例外，它们正是需要清理的对象
	out.erase(std::remove_if(out.begin(), out.end(), [](const ProgramInfo& p) {
		return p.installLocation.IsEmpty() && p.kind != ProgramKind::Orphan;
	}), out.end());

	// MSI 在前，EXE 其次，残留条目最后；同类按名称排序
	std::sort(out.begin(), out.end(), [](const ProgramInfo& a, const ProgramInfo& b) {
		int ra = KindRank(a.kind), rb = KindRank(b.kind);
		if (ra != rb)
			return ra < rb;
		return a.name.CompareNoCase(b.name) < 0;
	});
	return out;
}

CString UninstallOne(const ProgramInfo& p, RunContext& ctx)
{
	CLogger& log = *ctx.log;
	log.Write(L"==== %s [%s] %s", p.name.GetString(), KindText(p.kind),
		p.productCode.IsEmpty() ? p.arpSubKey.GetString() : p.productCode.GetString());

	CString result;
	switch (p.kind)
	{
	case ProgramKind::Msi: result = UninstallMsi(p, ctx); break;
	case ProgramKind::Exe: result = UninstallExe(p, ctx); break;
	default:               result = RemoveOrphan(p, ctx); break;
	}
	log.Write(L"  结果：%s", result.GetString());
	return result;
}
