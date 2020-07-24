/* $Id$ */
/** @file
 * DnD - Directory handling.
 */

/*
 * Copyright (C) 2014-2020 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#define LOG_GROUP LOG_GROUP_GUEST_DND
#include <VBox/GuestHost/DragAndDrop.h>

#include <iprt/assert.h>
#include <iprt/dir.h>
#include <iprt/err.h>
#include <iprt/file.h>
#include <iprt/mem.h>
#include <iprt/path.h>
#include <iprt/string.h>

#include <VBox/log.h>


/*********************************************************************************************************************************
*   Prototypes                                                                                                                   *
*********************************************************************************************************************************/
static int dndDroppedFilesCloseInternal(PDNDDROPPEDFILES pDF);


static int dndDroppedFilesInitInternal(PDNDDROPPEDFILES pDF)
{
    pDF->m_fOpen    = 0;
    pDF->m_hDir     = NIL_RTDIR;
    pDF->pszPathAbs = NULL;

    RTListInit(&pDF->m_lstDirs);
    RTListInit(&pDF->m_lstFiles);

    return VINF_SUCCESS;
}

int DnDDroppedFilesInitEx(PDNDDROPPEDFILES pDF,
                          const char *pszPath, DNDURIDROPPEDFILEFLAGS fFlags /* = DNDURIDROPPEDFILE_FLAGS_NONE */)
{
    int rc = dndDroppedFilesInitInternal(pDF);
    if (RT_FAILURE(rc))
        return rc;

    return DnDDroppedFilesOpenEx(pDF, pszPath, fFlags);
}

int DnDDroppedFilesInit(PDNDDROPPEDFILES pDF)
{
    return dndDroppedFilesInitInternal(pDF);
}

void DnDDroppedFilesDestroy(PDNDDROPPEDFILES pDF)
{
    /* Only make sure to not leak any handles and stuff, don't delete any
     * directories / files here. */
    dndDroppedFilesCloseInternal(pDF);

    RTStrFree(pDF->pszPathAbs);
    pDF->pszPathAbs = NULL;
}

/**
 * Adds a file reference to a dropped files directory.
 *
 * @returns VBox status code.
 * @param   pszFile             Path of file entry to add.
 */
int DnDDroppedFilesAddFile(PDNDDROPPEDFILES pDF, const char *pszFile)
{
    AssertPtrReturn(pszFile, VERR_INVALID_POINTER);

    PDNDDROPPEDFILESENTRY pEntry = (PDNDDROPPEDFILESENTRY)RTMemAlloc(sizeof(DNDDROPPEDFILESENTRY));
    if (!pEntry)
        return VERR_NO_MEMORY;

    pEntry->pszPath = RTStrDup(pszFile);
    if (pEntry->pszPath)
    {
        RTListAppend(&pDF->m_lstFiles, &pEntry->Node);
        return VINF_SUCCESS;
    }

    RTMemFree(pEntry);
    return VERR_NO_MEMORY;
}

/**
 * Adds a directory reference to a dropped files directory.
 * Note: This does *not* (recursively) add sub entries.
 *
 * @returns VBox status code.
 * @param   pszDir              Path of directory entry to add.
 */
int DnDDroppedFilesAddDir(PDNDDROPPEDFILES pDF, const char *pszDir)
{
    AssertPtrReturn(pszDir, VERR_INVALID_POINTER);

    PDNDDROPPEDFILESENTRY pEntry = (PDNDDROPPEDFILESENTRY)RTMemAlloc(sizeof(DNDDROPPEDFILESENTRY));
    if (!pEntry)
        return VERR_NO_MEMORY;

    pEntry->pszPath = RTStrDup(pszDir);
    if (pEntry->pszPath)
    {
        RTListAppend(&pDF->m_lstDirs, &pEntry->Node);
        return VINF_SUCCESS;
    }

    RTMemFree(pEntry);
    return VERR_NO_MEMORY;
}

/**
 * Closes the dropped files directory handle, internal version.
 *
 * @returns VBox status code.
 */
static int dndDroppedFilesCloseInternal(PDNDDROPPEDFILES pDF)
{
    int rc;
    if (pDF->m_hDir != NULL)
    {
        rc = RTDirClose(pDF->m_hDir);
        if (RT_SUCCESS(rc))
            pDF->m_hDir = NULL;
    }
    else
        rc = VINF_SUCCESS;

    LogFlowFuncLeaveRC(rc);
    return rc;
}

/**
 * Closes the dropped files directory handle.
 *
 * @returns VBox status code.
 */
int DnDDroppedFilesClose(PDNDDROPPEDFILES pDF)
{
    return dndDroppedFilesCloseInternal(pDF);
}

/**
 * Returns the absolute path of the dropped files directory.
 *
 * @returns Pointer to absolute path of the dropped files directory.
 */
const char *DnDDroppedFilesGetDirAbs(PDNDDROPPEDFILES pDF)
{
    return pDF->pszPathAbs;
}

/**
 * Returns whether the dropped files directory has been opened or not.
 *
 * @returns \c true if open, \c false if not.
 */
bool DnDDroppedFilesIsOpen(PDNDDROPPEDFILES pDF)
{
    return (pDF->m_hDir != NULL);
}

/**
 * Opens (creates) the dropped files directory.
 *
 * @returns VBox status code.
 * @param   pszPath             Absolute path where to create the dropped files directory.
 * @param   fFlags              Dropped files flags to use for this directory.
 */
int DnDDroppedFilesOpenEx(PDNDDROPPEDFILES pDF,
                          const char *pszPath, DNDURIDROPPEDFILEFLAGS fFlags /* = DNDURIDROPPEDFILE_FLAGS_NONE */)
{
    AssertPtrReturn(pszPath, VERR_INVALID_POINTER);
    AssertReturn(fFlags == 0, VERR_INVALID_PARAMETER); /* Flags not supported yet. */

    int rc;

    do
    {
        char szDropDir[RTPATH_MAX];
        RTStrPrintf(szDropDir, sizeof(szDropDir), "%s", pszPath);

        /** @todo On Windows we also could use the registry to override
         *        this path, on Posix a dotfile and/or a guest property
         *        can be used. */

        /* Append our base drop directory. */
        rc = RTPathAppend(szDropDir, sizeof(szDropDir), "VirtualBox Dropped Files"); /** @todo Make this tag configurable? */
        if (RT_FAILURE(rc))
            break;

        /* Create it when necessary. */
        if (!RTDirExists(szDropDir))
        {
            rc = RTDirCreateFullPath(szDropDir, RTFS_UNIX_IRWXU);
            if (RT_FAILURE(rc))
                break;
        }

        /* The actually drop directory consist of the current time stamp and a
         * unique number when necessary. */
        char szTime[64];
        RTTIMESPEC time;
        if (!RTTimeSpecToString(RTTimeNow(&time), szTime, sizeof(szTime)))
        {
            rc = VERR_BUFFER_OVERFLOW;
            break;
        }

        rc = DnDPathSanitizeFileName(szTime, sizeof(szTime));
        if (RT_FAILURE(rc))
            break;

        rc = RTPathAppend(szDropDir, sizeof(szDropDir), szTime);
        if (RT_FAILURE(rc))
            break;

        /* Create it (only accessible by the current user) */
        rc = RTDirCreateUniqueNumbered(szDropDir, sizeof(szDropDir), RTFS_UNIX_IRWXU, 3, '-');
        if (RT_SUCCESS(rc))
        {
            RTDIR hDir;
            rc = RTDirOpen(&hDir, szDropDir);
            if (RT_SUCCESS(rc))
            {
                pDF->pszPathAbs = RTStrDup(szDropDir);
                AssertPtrBreakStmt(pDF->pszPathAbs, rc = VERR_NO_MEMORY);
                pDF->m_hDir     = hDir;
                pDF->m_fOpen    = fFlags;
            }
        }

    } while (0);

    LogFlowFuncLeaveRC(rc);
    return rc;
}

/**
 * Opens (creates) the dropped files directory in the system's temp directory.
 *
 * @returns VBox status code.
 * @param   fFlags              Dropped files flags to use for this directory.
 */
int DnDDroppedFilesOpenTemp(PDNDDROPPEDFILES pDF, DNDURIDROPPEDFILEFLAGS fFlags)
{
    AssertReturn(fFlags == 0, VERR_INVALID_PARAMETER); /* Flags not supported yet. */

    /*
     * Get the user's temp directory. Don't use the user's root directory (or
     * something inside it) because we don't know for how long/if the data will
     * be kept after the guest OS used it.
     */
    char szTemp[RTPATH_MAX];
    int rc = RTPathTemp(szTemp, sizeof(szTemp));
    if (RT_SUCCESS(rc))
        rc = DnDDroppedFilesOpenEx(pDF, szTemp, fFlags);

    return rc;
}

static void dndDroppedFilesEntryFree(PDNDDROPPEDFILESENTRY pEntry)
{
    if (!pEntry)
        return;
    RTStrFree(pEntry->pszPath);
    RTListNodeRemove(&pEntry->Node);
    RTMemFree(pEntry);
}

static void dndDroppedFilesResetList(PRTLISTANCHOR pListAnchor)
{
    PDNDDROPPEDFILESENTRY pEntryCur, pEntryNext;
    RTListForEachSafe(pListAnchor, pEntryCur, pEntryNext, DNDDROPPEDFILESENTRY, Node)
        dndDroppedFilesEntryFree(pEntryCur);
    Assert(RTListIsEmpty(pListAnchor));
}

/**
 * Resets a droppped files directory.
 *
 * @returns VBox status code.
 * @param   fDelete             Whether to physically delete the directory and its content
 *                              or just clear the internal references.
 */
int DnDDroppedFilesReset(PDNDDROPPEDFILES pDF, bool fDelete)
{
    int rc = dndDroppedFilesCloseInternal(pDF);
    if (RT_SUCCESS(rc))
    {
        if (fDelete)
        {
            rc = DnDDroppedFilesRollback(pDF);
        }
        else
        {
            dndDroppedFilesResetList(&pDF->m_lstDirs);
            dndDroppedFilesResetList(&pDF->m_lstFiles);
        }
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

/**
 * Re-opens a droppes files directory.
 *
 * @returns VBox status code, or VERR_NOT_FOUND if the dropped files directory has not been opened before.
 */
int DnDDroppedFilesReopen(PDNDDROPPEDFILES pDF)
{
    if (!pDF->pszPathAbs)
        return VERR_NOT_FOUND;

    return DnDDroppedFilesOpenEx(pDF, pDF->pszPathAbs, pDF->m_fOpen);
}

/**
 * Performs a rollback of a dropped files directory.
 * This cleans the directory by physically deleting all files / directories which have been added before.
 *
 * @returns VBox status code.
 */
int DnDDroppedFilesRollback(PDNDDROPPEDFILES pDF)
{
    if (!pDF->pszPathAbs)
        return VINF_SUCCESS;

    int rc = VINF_SUCCESS;

    /* Rollback by removing any stuff created.
     * Note: Only remove empty directories, never ever delete
     *       anything recursive here! Steam (tm) knows best ... :-) */
    int rc2;
    PDNDDROPPEDFILESENTRY pEntryCur, pEntryNext;
    RTListForEachSafe(&pDF->m_lstFiles, pEntryCur, pEntryNext, DNDDROPPEDFILESENTRY, Node)
    {
        rc2 = RTFileDelete(pEntryCur->pszPath);
        if (RT_SUCCESS(rc2))
            dndDroppedFilesEntryFree(pEntryCur);
        else if (RT_SUCCESS(rc))
           rc = rc2;
        /* Keep going. */
    }

    RTListForEachSafe(&pDF->m_lstDirs, pEntryCur, pEntryNext, DNDDROPPEDFILESENTRY, Node)
    {
        rc2 = RTDirRemove(pEntryCur->pszPath);
        if (RT_SUCCESS(rc2))
            dndDroppedFilesEntryFree(pEntryCur);
        else if (RT_SUCCESS(rc))
            rc = rc2;
        /* Keep going. */
    }

    if (RT_SUCCESS(rc))
    {
        rc2 = dndDroppedFilesCloseInternal(pDF);
        if (RT_SUCCESS(rc2))
        {
            /* Try to remove the empty root dropped files directory as well.
             * Might return VERR_DIR_NOT_EMPTY or similar. */
            rc2 = RTDirRemove(pDF->pszPathAbs);
        }
        if (RT_SUCCESS(rc))
            rc = rc2;
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

