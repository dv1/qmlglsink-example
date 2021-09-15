import QtQuick 2.0
import QtQuick.Layouts 1.3
import QtQuick.Window 2.0
import org.freedesktop.gstreamer.GLVideoItem 1.0


Window {
	id: window
	visible: true
	width: 1280
	height: 720
	property var subtitle: ""
	onSubtitleChanged: {
		subtitleTimer.stop();
		subtitleTimer.interval = Math.max(subtitle.length * 80, 1000);
		subtitleItem.visible = true;
		subtitleTimer.start();

	}

	GstGLVideoItem {
		objectName: "videoItem"
		anchors.fill: parent
		z: 1 // Set z to 1 to keep the video item below the subtitle item
	}

	Timer {
		id: subtitleTimer
		interval: 300
		running: false
		repeat: false
		onTriggered: {
			if (subtitle !== "")
				subtitle = "";
		}
	}

	Text {
		id: subtitleItem
		objectName: "subtitleItem"
		visible: false
		text: window.subtitle
		textFormat: Text.StyledText
		height: parent.height / 5
		color: "white"
		font.pixelSize: parent.height / 20
		style: Text.Outline
		styleColor: "black"
		horizontalAlignment: Text.AlignHCenter
		verticalAlignment: Text.AlignVCenter
		anchors.left: parent.left
		anchors.leftMargin: parent.width / 10
		anchors.right: parent.right
		anchors.rightMargin: parent.width / 10
		anchors.bottom: parent.bottom
		anchors.bottomMargin: parent.height / 10
		z: 2 // Set z to 2 to keep the subtitle item above the video item
	}
}
