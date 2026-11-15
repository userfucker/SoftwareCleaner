// SoftwareCleanerDlg.cpp: 实现文件
//

#include "pch.h"
#include "framework.h"
#include "SoftwareCleaner.h"
#include "SoftwareCleanerDlg.h"
#include "afxdialogex.h"

#include <memory>
#include <shlobj.h>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

enum { COL_NO, COL_NAME, COL_LOCATION, COL_KIND, COL_PUBLISHER, COL_VERSION, COL_STATUS };

CSoftwareCleanerDlg::CSoftwareCleanerDlg(CWnd* pParent /*=nullptr*/)
	: CDialogEx(IDD_SOFTWARECLEANER_DIALOG, pParent)
{
	m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
}

void CSoftwareCleanerDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_LIST_PROGRAMS, m_list);
}

BEGIN_MESSAGE_MAP(CSoftwareCleanerDlg, CDialogEx)
	ON_WM_PAINT()
	ON_WM_QUERYDRAGICON()
	ON_BN_CLICKED(IDC_BTN_CHECKALL, &CSoftwareCleanerDlg::OnBnClickedCheckAll)
	ON_BN_CLICKED(IDC_BTN_SEARCH, &CSoftwareCleanerDlg::OnBnClickedSearch)
	ON_BN_CLICKED(IDC_BTN_UNINSTALL, &CSoftwareCleanerDlg::OnBnClickedUninstall)
	ON_MESSAGE(WM_APP_ITEM_STATUS, &CSoftwareCleanerDlg::OnItemStatus)
	ON_MESSAGE(WM_APP_WORKER_DONE, &CSoftwareCleanerDlg::OnWorkerDone)
	ON_MESSAGE(WM_APP_CONFIRM, &CSoftwareCleanerDlg::OnConfirm)
END_MESSAGE_MAP()


BOOL CSoftwareCleanerDlg::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	SetIcon(m_hIcon, TRUE);
	SetIcon(m_hIcon, FALSE);

	m_list.ModifyStyle(LVS_TYPEMASK, LVS_REPORT | LVS_SHOWSELALWAYS);
	m_list.SetExtendedStyle(LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
	m_list.InsertColumn(COL_NO, L"序号", LVCFMT_LEFT, 50);
	m_list.InsertColumn(COL_NAME, L"程序名称", LVCFMT_LEFT, 220);
	m_list.InsertColumn(COL_LOCATION, L"安装位置", LVCFMT_LEFT, 230);
	m_list.InsertColumn(COL_KIND, L"类型", LVCFMT_LEFT, 60);
	m_list.InsertColumn(COL_PUBLISHER, L"发布者", LVCFMT_LEFT, 110);
	m_list.InsertColumn(COL_VERSION, L"版本号", LVCFMT_LEFT, 90);
	m_list.InsertColumn(COL_STATUS, L"状态", LVCFMT_LEFT, 150);

	WCHAR exe[MAX_PATH] = {};
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	CString dir(exe);
	m_logDir = dir.Left(dir.ReverseFind(L'\\')) + L"\\Logs";
	CreateDirectoryW(m_logDir, nullptr);
	m_log.Open(m_logDir + L"\\" + CTime::GetCurrentTime().Format(L"%Y%m%d_%H%M%S") + L".log");

	if (!IsUserAnAdmin())
		MessageBox(L"当前没有以管理员身份运行，大部分程序将无法卸载。\n请右键程序，选择“以管理员身份运行”。", L"提示", MB_ICONWARNING);

	SetStatus(L"准备");
	return TRUE;
}

void CSoftwareCleanerDlg::OnPaint()
{
	if (IsIconic())
	{
		CPaintDC dc(this);
		SendMessage(WM_ICONERASEBKGND, reinterpret_cast<WPARAM>(dc.GetSafeHdc()), 0);

		int cxIcon = GetSystemMetrics(SM_CXICON);
		int cyIcon = GetSystemMetrics(SM_CYICON);
		CRect rect;
		GetClientRect(&rect);
		int x = (rect.Width() - cxIcon + 1) / 2;
		int y = (rect.Height() - cyIcon + 1) / 2;
		dc.DrawIcon(x, y, m_hIcon);
	}
	else
	{
		CDialogEx::OnPaint();
	}
}

HCURSOR CSoftwareCleanerDlg::OnQueryDragIcon()
{
	return static_cast<HCURSOR>(m_hIcon);
}

// 按回车不关闭窗口
void CSoftwareCleanerDlg::OnOK()
{
}

void CSoftwareCleanerDlg::OnCancel()
{
	if (m_busy)
	{
		MessageBox(L"正在卸载，请等待完成后再关闭。", L"提示", MB_ICONINFORMATION);
		return;
	}
	CDialogEx::OnCancel();
}

void CSoftwareCleanerDlg::SetStatus(const CString& text)
{
	SetDlgItemText(IDC_STATIC_STATUS, text);
	if (CWnd* w = GetDlgItem(IDC_STATIC_STATUS))
		w->UpdateWindow();
}

void CSoftwareCleanerDlg::SetBusy(bool busy)
{
	m_busy = busy;
	const UINT ids[] = { IDC_BTN_CHECKALL, IDC_BTN_SEARCH, IDC_BTN_UNINSTALL, IDCANCEL };
	for (UINT id : ids)
		if (CWnd* w = GetDlgItem(id))
			w->EnableWindow(!busy);
}

void CSoftwareCleanerDlg::SyncChecks()
{
	for (int i = 0; i < m_list.GetItemCount(); ++i)
	{
		size_t idx = m_list.GetItemData(i);
		if (idx < m_programs.size())
			m_programs[idx].checked = m_list.GetCheck(i) != FALSE;
	}
}

void CSoftwareCleanerDlg::FillList()
{
	m_list.SetRedraw(FALSE);
	m_list.DeleteAllItems();

	int row = 0;
	for (size_t i = 0; i < m_programs.size(); ++i)
	{
		const ProgramInfo& p = m_programs[i];
		CString no;
		no.Format(L"%d", row + 1);
		CString kind = KindText(p.kind);
		if (p.kind == ProgramKind::Msi && p.context != MSIINSTALLCONTEXT_MACHINE)
			kind += L"·用户";

		int r = m_list.InsertItem(row, no);
		m_list.SetItemText(r, COL_NAME, p.name);
		m_list.SetItemText(r, COL_LOCATION, p.installLocation);
		m_list.SetItemText(r, COL_KIND, kind);
		m_list.SetItemText(r, COL_PUBLISHER, p.publisher);
		m_list.SetItemText(r, COL_VERSION, p.version);
		m_list.SetItemText(r, COL_STATUS, p.status);
		m_list.SetItemData(r, static_cast<DWORD_PTR>(i));
		m_list.SetCheck(r, p.checked);
		++row;
	}

	m_list.SetRedraw(TRUE);
	m_list.Invalidate();

	CString s;
	s.Format(L"共 %d 个程序", row);
	SetStatus(s);
}

// 搜索：扫描已安装的程序，重建列表
void CSoftwareCleanerDlg::OnBnClickedSearch()
{
	if (m_busy)
		return;
	SetStatus(L"正在搜索已安装的程序…");
	CWaitCursor wait;
	m_programs = EnumeratePrograms();
	m_log.Write(L"搜索完成，共 %d 个程序", static_cast<int>(m_programs.size()));
	FillList();
}

// 当前列表里有没勾上的就全部勾上，否则全部取消
void CSoftwareCleanerDlg::OnBnClickedCheckAll()
{
	int n = m_list.GetItemCount();
	bool anyUnchecked = false;
	for (int i = 0; i < n && !anyUnchecked; ++i)
		anyUnchecked = !m_list.GetCheck(i);
	for (int i = 0; i < n; ++i)
		m_list.SetCheck(i, anyUnchecked);
}

void CSoftwareCleanerDlg::OnBnClickedUninstall()
{
	SyncChecks();
	m_queue.clear();
	for (size_t i = 0; i < m_programs.size(); ++i)
		if (m_programs[i].checked)
			m_queue.push_back(i);

	if (m_queue.empty())
	{
		MessageBox(L"请先勾选要卸载的程序。", L"提示", MB_ICONINFORMATION);
		return;
	}

	CString q;
	q.Format(L"将依次卸载勾选的 %d 个程序。\n\n正常卸载失败的程序，会再询问你是否强制删除。\n是否开始？",
		static_cast<int>(m_queue.size()));
	if (MessageBox(q, L"确认", MB_YESNO | MB_ICONQUESTION) != IDYES)
		return;

	m_log.Write(L"开始批量卸载，共 %d 个", static_cast<int>(m_queue.size()));
	SetBusy(true);
	if (!AfxBeginThread(WorkerProc, this))
	{
		SetBusy(false);
		MessageBox(L"无法启动卸载线程。", L"错误", MB_ICONERROR);
	}
}

UINT AFX_CDECL CSoftwareCleanerDlg::WorkerProc(LPVOID param)
{
	auto* dlg = static_cast<CSoftwareCleanerDlg*>(param);
	HWND hwnd = dlg->GetSafeHwnd();

	RunContext ctx;
	ctx.log = &dlg->m_log;
	ctx.logDir = dlg->m_logDir;
	ctx.confirm = [hwnd](const CString& text) {
		return ::SendMessage(hwnd, WM_APP_CONFIRM, 0, reinterpret_cast<LPARAM>(&text)) == IDYES;
	};

	for (size_t idx : dlg->m_queue)
	{
		ctx.progress = [hwnd, idx](const CString& s) {
			::PostMessage(hwnd, WM_APP_ITEM_STATUS, idx, reinterpret_cast<LPARAM>(new CString(s)));
		};
		ctx.progress(L"正在卸载…");
		ctx.progress(UninstallOne(dlg->m_programs[idx], ctx));
	}

	::PostMessage(hwnd, WM_APP_WORKER_DONE, 0, 0);
	return 0;
}

LRESULT CSoftwareCleanerDlg::OnItemStatus(WPARAM wParam, LPARAM lParam)
{
	std::unique_ptr<CString> text(reinterpret_cast<CString*>(lParam));
	size_t idx = static_cast<size_t>(wParam);
	if (idx >= m_programs.size())
		return 0;

	m_programs[idx].status = *text;

	LVFINDINFO fi = {};
	fi.flags = LVFI_PARAM;
	fi.lParam = static_cast<LPARAM>(idx);
	int row = m_list.FindItem(&fi);
	if (row >= 0)
	{
		m_list.SetItemText(row, COL_STATUS, *text);
		m_list.EnsureVisible(row, FALSE);
	}
	SetStatus(m_programs[idx].name + L"：" + *text);
	return 0;
}

LRESULT CSoftwareCleanerDlg::OnWorkerDone(WPARAM, LPARAM)
{
	SetBusy(false);
	m_log.Write(L"批量卸载结束");
	SetStatus(L"全部处理完成。日志：" + m_log.Path());
	MessageBox(L"全部处理完成。\n点“搜索”可以刷新列表。\n\n日志文件：\n" + m_log.Path(), L"完成", MB_ICONINFORMATION);
	return 0;
}

LRESULT CSoftwareCleanerDlg::OnConfirm(WPARAM, LPARAM lParam)
{
	const CString* text = reinterpret_cast<const CString*>(lParam);
	return MessageBox(*text, L"请确认", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
}
