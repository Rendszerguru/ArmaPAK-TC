#ifndef __WCXHEAD_H__
#define __WCXHEAD_H__

#ifdef __cplusplus
extern "C" {
#endif 

/* DCPCALL definition */
#ifdef _WIN32
  #define DCPCALL __stdcall
#else
  #define DCPCALL
#endif

#include <windows.h>   // HANDLE, CreateDirectory, stb.
#include <wchar.h>     // WCHAR
#include <tchar.h>     // TCHAR
#include <stdint.h>    // int32_t, stb.
#include <stdio.h>

#define E_END_ARCHIVE     10            /* No more files in archive */
#define E_NO_MEMORY       11            /* Not enough memory */
#define E_BAD_DATA        12            /* Data is bad */
#define E_BAD_ARCHIVE     13            /* CRC error in archive data */
#define E_UNKNOWN_FORMAT  14            /* Archive format unknown */
#define E_EOPEN           15            /* Cannot open existing file */
#define E_ECREATE         16            /* Cannot create file */
#define E_ECLOSE          17            /* Error closing file */
#define E_EREAD           18            /* Error reading from file */
#define E_EWRITE          19            /* Error writing to file */
#define E_SMALL_BUF       20            /* Buffer too small */
#define E_EABORTED        21            /* Function aborted by user */
#define E_NO_FILES        22            /* No files found */
#define E_TOO_MANY_FILES  23            /* Too many files to pack */
#define E_NOT_SUPPORTED   24            /* Function not supported */

#define E_SUCCESS           0

#define E_NO_MORE_FILES     0      /* No match found */
#define E_FILESEARCHOK      1      /* Match found */


/* flags for unpacking */
#define PK_OM_LIST          0
#define PK_OM_EXTRACT       1

/* flags for ProcessFile */
#define PK_SKIP             0            /* Skip this file */
#define PK_TEST             1            /* Test file integrity */
#define PK_EXTRACT          2            /* Extract to disk */

/* Flags passed through ChangeVolProc */
#define PK_VOL_ASK          0            /* Ask user for location of next volume */
#define PK_VOL_NOTIFY       1            /* Notify app that next volume will be unpacked */

/* Flags for packing */

/* For PackFiles */
#define PK_PACK_MOVE_FILES  1    /* Delete original after packing        */
#define PK_PACK_SAVE_PATHS  2    /* Save path names of files             */

/* Returned by GetPackCaps */
#define PK_CAPS_NEW         1    /* Can create new archives              */
#define PK_CAPS_MODIFY      2    /* Can modify exisiting archives        */
#define PK_CAPS_MULTIPLE    4    /* Archive can contain multiple files   */
#define PK_CAPS_DELETE      8    /* Can delete files                     */
#define PK_CAPS_OPTIONS    16    /* Has options dialog                   */
#define PK_CAPS_MEMPACK    32    /* Supports packing in memory           */
#define PK_CAPS_BY_CONTENT 64    /* Detect archive type by content       */
#define PK_CAPS_SEARCHTEXT 128   /* Allow searching for text in archives */

#define PK_CAPS_HIDE       256   /* Show as normal files (hide packer    */

/* Flags for packing in memory */
#define MEM_OPTIONS_WANTHEADERS 1  /* Return archive headers with packed data */

//{Errors returned by PackToMem}
#define MEMPACK_OK          0    /* Function call finished OK, but there is more data */
#define MEMPACK_DONE        1    /* Function call finished OK, there is no more data  */


typedef struct {
	char ArcName[260];
	char FileName[260];
	int Flags;
	int PackSize;
	int UnpSize;
	int HostOS;
	int FileCRC;
	int FileTime;
	int UnpVer;
	int Method;
	int FileAttr;
	char* CmtBuf;
	int CmtBufSize;
	int CmtSize;
	int CmtState;
  } tHeaderData;

typedef struct {
	char ArcName[1024];
	char FileName[1024];
	int Flags;
	unsigned int PackSize;
	unsigned int PackSizeHigh;
	unsigned int UnpSize;
	unsigned int UnpSizeHigh;
	int HostOS;
	int FileCRC;
	int FileTime;
	int UnpVer;
	int Method;
	int FileAttr;
	char* CmtBuf;
	int CmtBufSize;
	int CmtSize;
	int CmtState;
	char Reserved[1024];
  } tHeaderDataEx;

typedef struct {
	WCHAR ArcName[1024];
	WCHAR FileName[1024];
	int Flags;
	unsigned int PackSize;
	unsigned int PackSizeHigh;
	unsigned int UnpSize;
	unsigned int UnpSizeHigh;
	int HostOS;
	int FileCRC;
	int FileTime;
	int UnpVer;
	int Method;
	int FileAttr;
	char* CmtBuf;
	int CmtBufSize;
	int CmtSize;
	int CmtState;
	char Reserved[1024];
  } tHeaderDataExW;

typedef struct {
	char* ArcName;
	int OpenMode;
	int OpenResult;
	char* CmtBuf;
	int CmtBufSize;
	int CmtSize;
	int CmtState;
  } tOpenArchiveData;

typedef struct {
	WCHAR* ArcName;
	int OpenMode;
	int OpenResult;
	WCHAR* CmtBuf;
	int CmtBufSize;
	int CmtSize;
	int CmtState;
  } tOpenArchiveDataW;

typedef struct {
	int size;
	DWORD PluginInterfaceVersionLow;
	DWORD PluginInterfaceVersionHi;
	char DefaultIniName[MAX_PATH];
} PackDefaultParamStruct;

/* Definition of callback functions called by the DLL
Ask to swap disk for multi-volume archive */
typedef int (__stdcall *tChangeVolProc)(char *ArcName,int Mode);
/* Notify that data is processed - used for progress dialog */
typedef int (__stdcall *tProcessDataProc)(char *FileName,int Size);

HANDLE DCPCALL OpenArchive(tOpenArchiveData *ArchiveData);
HANDLE DCPCALL OpenArchiveW(tOpenArchiveDataW *ArchiveDataW);
int DCPCALL ReadHeader(HANDLE hArcData, tHeaderData *HeaderData);
int DCPCALL ReadHeaderEx(HANDLE hArcData, tHeaderDataEx *HeaderDataEx);
int DCPCALL ReadHeaderExW(HANDLE hArcData, tHeaderDataExW *HeaderDataEx);
int DCPCALL ProcessFile(HANDLE hArcData, int Operation, char *DestPath, char *DestName);
int DCPCALL ProcessFileW(HANDLE hArcData, int Operation, const wchar_t *DestPath, const wchar_t *DestName);
int DCPCALL CloseArchive(HANDLE hArcData);

void __stdcall SetChangeVolProc (HANDLE hArcData, tChangeVolProc pChangeVolProc1);
void __stdcall SetProcessDataProc (HANDLE hArcData, tProcessDataProc pProcessDataProc);

}

#endif
