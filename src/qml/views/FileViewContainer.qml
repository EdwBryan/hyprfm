import QtQuick
import HyprFM

Item {
    id: root

    // "grid" | "detailed" | "miller"
    property string viewMode: "grid"
    property var fileModel: null
    property string currentPath: ""

    // Zoom state persisted in session.json. The views own their value; these
    // mirror it up so the session save always sees the latest, and apply the
    // remembered value once at startup (0 = none saved, keep the default).
    // zoomRestored suppresses the mirror while the initial value settles,
    // otherwise the first default 7/28/28 would overwrite the restored values.
    property int gridColumns: gridView.columnCount
    property int rowHeightDetailed: detailedView.rowHeight
    property int rowHeightMiller: millerView.rowHeight
    property bool zoomRestored: false

    onGridColumnsChanged: if (root.zoomRestored) sessionState.gridColumns = root.gridColumns
    onRowHeightDetailedChanged: if (root.zoomRestored) sessionState.rowHeightDetailed = root.rowHeightDetailed
    onRowHeightMillerChanged: if (root.zoomRestored) sessionState.rowHeightMiller = root.rowHeightMiller

    Component.onCompleted: {
        if (sessionState.gridColumns > 0)
            gridView.columnCount = Math.max(gridView.minColumns,
                Math.min(gridView.maxColumns, sessionState.gridColumns))
        if (sessionState.rowHeightDetailed > 0)
            detailedView.rowHeight = Math.max(detailedView.minRowHeight,
                Math.min(detailedView.maxRowHeight, sessionState.rowHeightDetailed))
        if (sessionState.rowHeightMiller > 0)
            millerView.rowHeight = Math.max(millerView.minRowHeight,
                Math.min(millerView.maxRowHeight, sessionState.rowHeightMiller))
        root.zoomRestored = true
    }

    signal fileActivated(string filePath, bool isDirectory)
    signal contextMenuRequested(string filePath, bool isDirectory, point position)
    signal selectionChanged()
    signal interactionStarted()
    signal transferRequested(var paths, string destinationPath, bool moveOperation)
    signal sortRequested(string column, bool ascending)

    function selectAll() {
        if (viewMode === "grid") gridView.selectAll()
        else if (viewMode === "miller") millerView.selectAll()
        else detailedView.selectAll()
    }

    function focusPath(path, reveal) {
        gridView.focusPath(path, reveal)
        detailedView.focusPath(path, reveal)
        millerView.focusPath(path, reveal)
    }

    // Expose sub-views so main.qml can access selection state
    property alias gridViewItem: gridView
    property alias detailedViewItem: detailedView
    property alias millerViewItem: millerView

    FileGridView {
        id: gridView
        anchors.fill: parent
        visible: root.viewMode === "grid"
        model: visible ? root.fileModel : null
        currentPath: root.currentPath

        onFileActivated: (fp, isDir) => root.fileActivated(fp, isDir)
        onContextMenuRequested: (fp, isDir, pos) => root.contextMenuRequested(fp, isDir, pos)
        onSelectedIndicesChanged: root.selectionChanged()
        onInteractionStarted: root.interactionStarted()
        onTransferRequested: (paths, destinationPath, moveOperation) => root.transferRequested(paths, destinationPath, moveOperation)
    }

    FileDetailedView {
        id: detailedView
        anchors.fill: parent
        visible: root.viewMode === "detailed"
        viewModel: visible ? root.fileModel : null
        currentPath: root.currentPath

        onFileActivated: (fp, isDir) => root.fileActivated(fp, isDir)
        onContextMenuRequested: (fp, isDir, pos) => root.contextMenuRequested(fp, isDir, pos)
        onSortRequested: (col, asc) => root.sortRequested(col, asc)
        onSelectedIndicesChanged: root.selectionChanged()
        onInteractionStarted: root.interactionStarted()
        onTransferRequested: (paths, destinationPath, moveOperation) => root.transferRequested(paths, destinationPath, moveOperation)
    }

    FileMillerView {
        id: millerView
        anchors.fill: parent
        visible: root.viewMode === "miller"
        fileModel: visible ? root.fileModel : null
        currentPath: root.currentPath

        onFileActivated: (fp, isDir) => root.fileActivated(fp, isDir)
        onContextMenuRequested: (fp, isDir, pos) => root.contextMenuRequested(fp, isDir, pos)
        onSelectionChanged: root.selectionChanged()
        onInteractionStarted: root.interactionStarted()
        onTransferRequested: (paths, destinationPath, moveOperation) => root.transferRequested(paths, destinationPath, moveOperation)
    }
}
