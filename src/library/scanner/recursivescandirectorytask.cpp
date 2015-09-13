#include <QDirIterator>

#include "library/scanner/recursivescandirectorytask.h"

#include "library/scanner/libraryscanner.h"
#include "library/scanner/importfilestask.h"
#include "util/timer.h"

RecursiveScanDirectoryTask::RecursiveScanDirectoryTask(
        LibraryScanner* pScanner, const ScannerGlobalPointer scannerGlobal,
        const QDir& dir, SecurityTokenPointer pToken)
        : ScannerTask(pScanner, scannerGlobal),
          m_dir(dir),
          m_pToken(pToken) {
    // Avoid revisiting this directory again during this scan. Using
    // the canonical path here is necessary to detect cyclic symbolic
    // links and exclude them from further scanning!
    m_scannerGlobal->setDirectoryScanned(m_dir.canonicalPath());
}

void RecursiveScanDirectoryTask::run() {
    ScopedTimer timer("RecursiveScanDirectoryTask::run");
    if (m_scannerGlobal->shouldCancel()) {
        setSuccess(false);
        return;
    }

    // For making the scanner slow
    //qDebug() << "Burn CPU";
    //for (int i = 0;i < 1000000000; i++) asm("nop");

    // Note, we save on filesystem operations (and random work) by initializing
    // a QDirIterator with a QDir instead of a QString -- but it inherits its
    // Filter from the QDir so we have to set it first. If the QDir has not done
    // any FS operations yet then this should be lightweight.
    m_dir.setFilter(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
    QDirIterator it(m_dir);

    QString currentFile;
    QFileInfo currentFileInfo;
    QLinkedList<QFileInfo> filesToImport;
    QLinkedList<QFileInfo> possibleCovers;
    QLinkedList<QDir> dirsToScan;
    QStringList newHashStr;

    // TODO(rryan) benchmark QRegExp copy versus QMutex/QRegExp in ScannerGlobal
    // versus slicing the extension off and checking for set/list containment.
    QRegExp supportedExtensionsRegex =
            m_scannerGlobal->supportedExtensionsRegex();
    QRegExp supportedCoverExtensionsRegex =
            m_scannerGlobal->supportedCoverExtensionsRegex();

    while (it.hasNext()) {
        currentFile = it.next();
        currentFileInfo = it.fileInfo();

        if (currentFileInfo.isFile()) {
            const QString& fileName = currentFileInfo.fileName();
            if (supportedExtensionsRegex.indexIn(fileName) != -1) {
                newHashStr.append(currentFile);
                filesToImport.append(currentFileInfo);
            } else if (supportedCoverExtensionsRegex.indexIn(fileName) != -1) {
                possibleCovers.append(currentFileInfo);
            }
        } else {
            // File is a directory
            if (m_scannerGlobal->directoryBlacklisted(currentFile)) {
                // Skip blacklisted directories
                continue;
            }
            const QDir currentDir(currentFile);
            const QString canonicalDirPath(currentDir.canonicalPath());
            if (m_scannerGlobal->isDirectoryScanned(canonicalDirPath)) {
                // Skip directories that have already been scanned
                continue;
            }
            // Add unvisited directories to our list of directories to scan.
            dirsToScan.append(currentDir);
            // Skip the iTunes Album Art Folder since it is probably a waste of
            // time.
        }
    }

    // Note: A hash of "0" is a real hash if the directory contains no files!
    // Calculate a hash of the directory's file list.
    int newHash = qHash(newHashStr.join(""));

    QString dirPath = m_dir.path();

    // Try to retrieve a hash from the last time that directory was scanned.
    int prevHash = m_scannerGlobal->directoryHashInDatabase(dirPath);
    bool prevHashExists = prevHash != -1;

    // Compare the hashes, and if they don't match, rescan the files in that
    // directory!
    if (prevHash != newHash) {
        // Rescan that mofo! If importing fails then the scan was cancelled so
        // we return immediately.
        if (!filesToImport.isEmpty()) {
            m_pScanner->queueTask(
                    new ImportFilesTask(m_pScanner, m_scannerGlobal, dirPath,
                                        newHash, prevHashExists, filesToImport,
                                        possibleCovers, m_pToken));
        } else {
            emit(directoryHashed(dirPath, !prevHashExists, newHash));
        }
    } else {
        emit(directoryUnchanged(dirPath));
    }

    // Process all of the sub-directories.
    foreach (const QDir& nextDir, dirsToScan) {
        m_pScanner->queueTask(
                new RecursiveScanDirectoryTask(m_pScanner, m_scannerGlobal,
                                               nextDir, m_pToken));
    }

    setSuccess(true);
}
