// SoftwareCleanerDlg.h: 头文件
//

#pragma once

#include <vector>
#include "UninstallEngine.h"

constexpr UINT WM_APP_ITEM_STATUS = WM_APP + 1;
constexpr UINT WM_APP_WORKER_DONE = WM_APP + 2;
constexpr UINT WM_APP_CONFIRM     = WM_APP + 3;

// CSoftwareCleanerDlg 对话框
class CSoftwareCleanerDlg : public CDialogEx
{
public:
	CSoftwareCleanerDlg(CWnd* pParent = nullptr);

#ifdef AFX_DESIGN_TIME
	enum { IDD = IDD_SOFTWARECLEANER_DIALOG };
#endif

protected:
	virtual void DoDataExchange(CDataExchange* pDX);
	virtual BOOL OnInitDialog();
	virtual void OnOK();
	virtual void OnCancel();

	afx_msg void OnPaint();
	afx_msg HCURSOR OnQueryDragIcon();
	afx_msg void OnBnClickedCheckAll();
	afx_msg void OnBnClickedSearch();
	afx_msg void OnBnClickedUninstall();
	afx_msg LRESULT OnItemStatus(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnWorkerDone(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnConfirm(WPARAM wParam, LPARAM lParam);
	DECLARE_MESSAGE_MAP()

private:
	static UINT AFX_CDECL WorkerProc(LPVOID param);
	void FillList();
	void SyncChecks();
	void SetBusy(bool busy);
	void SetStatus(const CString& text);

	HICON m_hIcon;
	CListCtrl m_list;
	std::vector<ProgramInfo> m_programs;
	std::vector<size_t> m_queue;
	CString m_logDir;
	CLogger m_log;
	bool m_busy = false;
};
