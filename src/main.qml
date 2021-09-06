import QtQuick 2.0
import QtQuick.Layouts 1.3
import QtQuick.Window 2.0
import org.freedesktop.gstreamer.GLVideoItem 1.0


Window {
	visible: true
	width: 1280
	height: 720

	GstGLVideoItem {
		objectName: "videoItem"
		anchors.fill: parent
	}
}
