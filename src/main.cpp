#include <assert.h>
#include <array>
#include <cstring>
#include <cerrno>
#include <map>

#include <gst/gst.h>
#include <gst/app/app.h>

#include <signal.h>
#include <unistd.h>

#include <QRunnable>
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQmlApplicationEngine>
#include <QCommandLineParser>
#include <QSocketNotifier>
#include <QString>

#include "ScopeGuard.hpp"


// Utility code to set up signal handlers to gracefully quit
// the application when these signals are caught. Most notably,
// this sets up a SIGINT signal handler to allow for quitting
// the application by pressing Ctrl+C in the terminal.

volatile sig_atomic_t signalFd = -1;

void sigHandler(int)
{
	if (signalFd != -1)
	{
		auto ret = write(signalFd, "1", 1);
		assert(ret >= 1);
	}
}

static constexpr std::array<int, 4> SignalsToHandle = {{ SIGINT, SIGTERM, SIGQUIT, SIGHUP }};

char const * signalString(int signal)
{
	switch (signal)
	{
		case SIGINT: return "SIGINT";
		case SIGTERM: return "SIGTERM";
		case SIGQUIT: return "SIGQUIT";
		case SIGHUP: return "SIGHUP";
		default: return "<unknown>";
	}
}

class Sighandler
{
public:
	Sighandler()
	{
	}

	~Sighandler()
	{
		for (auto &item : m_oldSigactions)
		{
			int signal = item.first;
			struct sigaction &oldSigaction = item.second;
			sigaction(signal, &oldSigaction, nullptr);
		}

		delete m_notifier;
		if (m_pipeFds[0] != -1)
			close(m_pipeFds[0]);
		if (m_pipeFds[1] != -1)
			close(m_pipeFds[1]);
		signalFd = -1;
	}

	bool setup(QWindow *window)
	{
		if (pipe(m_pipeFds.data()) == -1)
		{
			qCritical() << "Could not create signal pipe: " << std::strerror(errno);
			return false;
		}
		signalFd = m_pipeFds[1];

		m_notifier = new QSocketNotifier(m_pipeFds[0], QSocketNotifier::Read, nullptr);
		QObject::connect(m_notifier, &QSocketNotifier::activated, [this, window]() {
			if (signalFd < 0)
				return;

			char c;
			auto ret = read(m_pipeFds[0], &c, 1);
			if (ret >= 1)
			{
				qDebug() << "Signal caught, quitting";
				window->close();
			}
			else if (ret < 0)
			{
				qCritical() << "Error reading from signal pipe:" << std::strerror(errno) << " " << signalFd;
				window->close();
			}
		});

		for (int signal : SignalsToHandle)
		{
			struct sigaction oldSigaction;

			if (sigaction(signal, nullptr, &oldSigaction) < 0)
			{
				qCritical() << "Could not get old" << signalString(signal) << "signal handler:" << std::strerror(errno);
				return false;
			}

			if (oldSigaction.sa_handler != SIG_IGN)
			{
				struct sigaction newSigaction;

				sigemptyset(&newSigaction.sa_mask);
				newSigaction.sa_handler = sigHandler;
				newSigaction.sa_flags = SA_RESTART;
				sigfillset(&(newSigaction.sa_mask));

				if (sigaction(signal, &newSigaction, nullptr) < 0)
				{
					qCritical() << "Could not set up new" << signalString(signal) << "signal handler:" << std::strerror(errno);
					return false;
				}
			}

			m_oldSigactions[signal] = std::move(oldSigaction);
		}

		return true;
	}

private:
	std::array<int, 2> m_pipeFds = {{ -1, -1 }};
	QSocketNotifier *m_notifier = nullptr;

	std::map<int, struct sigaction> m_oldSigactions;
};


// Simple playbin based GStreamer pipeline.

class Pipeline
{
public:
	Pipeline()
	{
	}

	~Pipeline()
	{
		if (m_playbin == nullptr)
			return;

		// Stop playback by setting the pipeline to the NULL state.
		gst_element_set_state(m_playbin, GST_STATE_NULL);

		// Make sure the qmlglsink no longer uses the Qt widget
		// before the QML UI is torn down.
		if (m_qmlglsink != nullptr)
			g_object_set(m_qmlglsink, "widget", gpointer(nullptr), nullptr);

		// Deallocate the pipeline. We are not explictly deallocating
		// m_qmlglsink, since that one is taken care of by glsinkbin,
		// which in turn is taken care of by m_playbin.
		gst_object_unref(GST_OBJECT(m_playbin));
	}


	bool setup(QString inputUrl, QObject *qmlSubtitleItem)
	{
		// Scope guard to cleanup the pipeline in case setup fails.
		auto pipelineGuard = makeScopeGuard([&]() {
			if (m_playbin != nullptr)
			{
				gst_object_unref(GST_OBJECT(m_playbin));
				m_playbin = nullptr;
			}
		});


		// Store the pointer to be able to set its subtitle property later.
		m_qmlSubtitleItem = qmlSubtitleItem;


		// Create the pipeline.

		// Note that playbin is a fully featured pipeline element, and putting
		// it in a dedicated additional pipeline element is unnecessary, which
		// is why there's no gst_pipeline_new() call here.
		m_playbin = gst_element_factory_make("playbin", nullptr);
		if (m_playbin == nullptr)
		{
			qCritical() << "Could not create playbin element";
			return false;
		}

		GstElement *glsinkbin = nullptr;
		GstElement *subtitleAppsink = nullptr;

		// Scope guard to make sure the elements above are always
		// unref'd in case an error occurs. This guard is needed
		// until these elements are transferred over to playbin.
		auto elementUnrefGuard = makeScopeGuard([&]() {
			if (glsinkbin != nullptr)
				gst_object_unref(GST_OBJECT(glsinkbin));
			if (subtitleAppsink != nullptr)
				gst_object_unref(GST_OBJECT(subtitleAppsink));
		});

		// Create the glsinkbin. This will be used as the video sink by playbin.
		glsinkbin = gst_element_factory_make("glsinkbin", nullptr);
		if (glsinkbin == nullptr)
		{
			qCritical() << "Could not create glsinkbin element";
			return false;
		}

		// Create the appsink that will be used for extracting subtitles.
		subtitleAppsink = gst_element_factory_make("appsink", nullptr);
		if (subtitleAppsink == nullptr)
		{
			qCritical() << "Could not create subtitle appsink element";
			return false;
		}

		// Create the qmlglsink and assign it to the glsinkbin, which
		// takes ownership over that qmlglsink.
		m_qmlglsink = gst_element_factory_make("qmlglsink", nullptr);
		if (m_qmlglsink == nullptr)
		{
			qCritical() << "Could not create qmlglsink element";
			return false;
		}
		g_object_set(glsinkbin, "sink", m_qmlglsink, nullptr);

		// Set the glsinkbin as the video sink to use for playback. The flags
		// are set to 0x57, which disables all software based video postprocessing
		// (color balancing, deinterlacing ...) but keeps software based audio
		// postprocessing enabled. Disabling the video postprocessing is essential
		// on embedded platforms to minimize stutter (which is cause by a saturated CPU).
		// Also, set the subtitleAppsink as the "text sink" (aka the subtitle sink).
		g_object_set(
			m_playbin,
			"uri", inputUrl.toStdString().c_str(),
			"flags", gint(0x57),
			"video-sink", glsinkbin,
			"text-sink", subtitleAppsink,
			nullptr
		);

		// playbin owns the glsinkbin and subtitle appsink now.
		// The scope guard is no longer needed.
		elementUnrefGuard.dismiss();

		// Set the appsink callbacks to be informed whenever new subtitles are read.
		// These subtitles can then be displayed in QML.
		{
			GstAppSinkCallbacks subtitleAppsinkCallbacks = {};
			subtitleAppsinkCallbacks.new_sample = &staticOnNewSubtitle;

			gst_app_sink_set_callbacks(
				GST_APP_SINK(subtitleAppsink),
				&subtitleAppsinkCallbacks,
				gpointer(this),
				nullptr
			);

			// Further refine appsink behavior:
			// - Enable the drop property to make sure the appsink never blocks.
			//   If subtitles are not shown in time, we anyway do not want to
			//   show them anymore, so it is OK to drop stale subtitles.
			// - Set max-buffers to 1 since we do not want a queue of subtitles.
			g_object_set(
				G_OBJECT(subtitleAppsink),
				"drop", gboolean(TRUE),
				"max-buffers", guint(1),
				nullptr
			);
		}


		// Note that a proper application would also install a GStreamer bus watch
		// into the pipeline. And, that bus watch hooks into the GLib mainloop.
		// If Qt is built with Glib integration, then the Qt mainloop is built
		// upon the mainloop one, and the bus watch will "just work". If not,
		// then the bus watch would have to be attached to a dedicated mainloop
		// that runs in a separate thread. For sake of simplicity, this example
		// does not use a bus watch.


		// Dismiss the pipeline guard since setup completed successfully.
		pipelineGuard.dismiss();
		return true;
	}


	bool start(QQuickItem *videoItem)
	{
		assert(m_playbin != nullptr);
		assert(m_qmlglsink != nullptr);

		// Assign the GLVideoItem from the QML UI to the qmlglsink before the
		// pipeline is started.
		// We cast the videoItem pointer to gpointer to avoid compiler warnings
		// and to make sure the GObject property system works properly.
		g_object_set(m_qmlglsink, "widget", gpointer(videoItem), nullptr);

		if (gst_element_set_state(m_playbin, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
		{
			qCritical() << "Could not set pipeline state to PLAYING";
			return false;
		}

		return true;
	}


private:
	static GstFlowReturn staticOnNewSubtitle(GstAppSink *subtitleAppsink, gpointer userData)
	{
		Pipeline *self = reinterpret_cast<Pipeline *>(userData);

		// Extract the subtitle text from the GstBuffer inside the newest GstSample.

		GstSample *subtitleSample = gst_app_sink_pull_sample(subtitleAppsink);
		GstBuffer *subtitleBuffer = gst_sample_get_buffer(subtitleSample);

		GstMapInfo mapInfo;
		gst_buffer_map(subtitleBuffer, &mapInfo, GST_MAP_READ);
		auto guard = makeScopeGuard([&]() {
			gst_buffer_unmap(subtitleBuffer, &mapInfo);
		});

		// NOTE: Typically, the subtitle buffers do _not_ contain
		// a trailing nullbyte.
		QString subtitle = QString::fromUtf8(
			reinterpret_cast<char const *>(mapInfo.data),
			mapInfo.size
		);

		qDebug() << "Subtitle:" << subtitle;
		self->m_qmlSubtitleItem->setProperty("subtitle", subtitle);

		return GST_FLOW_OK;
	}

	GstElement *m_playbin = nullptr;
	GstElement *m_qmlglsink = nullptr;
	QObject *m_qmlSubtitleItem = nullptr;
};


// Helper class to start the pipeline once the scenegraph is up and running.

class SetPlayingJob
	: public QRunnable
{
public:
	explicit SetPlayingJob(Pipeline &pipeline, QQuickItem *qmlVideoItem, QCoreApplication &application)
		: m_pipeline(pipeline)
		, m_qmlVideoItem(qmlVideoItem)
		, m_application(application)
	{
	}

	void run() override
	{
		if (!m_pipeline.start(m_qmlVideoItem))
		{
			qCritical() << "Could not start pipeline; quitting";
			m_application.quit();
		}
	}

private:
	Pipeline &m_pipeline;
	QQuickItem *m_qmlVideoItem;
	QCoreApplication &m_application;
};


int main(int argc, char *argv[])
{
	{
		GError *error = nullptr;
		if (!gst_init_check(&argc, &argv, &error))
		{
			qCritical() << "Could not initialize GStreamer: " << error->message;
			g_error_free(error);
			return -1;
		}
	}


	// Scope guard to make sure GStreamer is always deinitialized when execution leaves this scope.
	// gst_deinit() must be called to let its tracing framework present results at the end.
	// To use tracing, run this binary with these environment variables:
	//   GST_TRACERS=leaks GST_DEBUG=GST_TRACER:7
	auto guard = makeScopeGuard([&]() {
		gst_deinit();
		qDebug() << "Application finished";
	});


	// The app must be present _before_ a QML engine is created!
	QGuiApplication app(argc, argv);

	Sighandler sighandler;


	// Handle command line arguments.

	QCommandLineParser cmdlineParser;
	cmdlineParser.setApplicationDescription("qmlglsink-test");

	QCommandLineOption helpOption = cmdlineParser.addHelpOption();
	QCommandLineOption inputFileOrUrlOption(QStringList() << "i" << "input", "Input file/URL to play", "input");
	cmdlineParser.addOption(inputFileOrUrlOption);
	QCommandLineOption runInFullScreenOption(QStringList() << "f" << "fullscreen", "Run application in fullscreen mode");
	cmdlineParser.addOption(runInFullScreenOption);

	if (!cmdlineParser.parse(app.arguments()))
	{
		qCritical() << cmdlineParser.errorText();
		qCritical() << "";
		cmdlineParser.showHelp();
		return -1;
	}

	if (cmdlineParser.isSet(helpOption))
	{
		cmdlineParser.showHelp();
		return -1;
	}

	if (!cmdlineParser.isSet(inputFileOrUrlOption))
	{
		qCritical() << "Input file/URL (-i) must be set!";
		return -1;
	}

	QString inputUrl = cmdlineParser.value(inputFileOrUrlOption);
	bool runInFullscreen = cmdlineParser.isSet(runInFullScreenOption);

	if (!gst_uri_is_valid(inputUrl.toStdString().c_str()))
	{
		GError *error = nullptr;
		gchar *uri = gst_filename_to_uri(inputUrl.toStdString().c_str(), &error);
		if (uri != nullptr)
		{
			qCritical() << "Input is not a valid URI; treated it as a filename, and converted it to file URI" << uri;
			inputUrl = uri;
			g_free(uri);
		}
		else
		{
			qCritical() << "Input is not a valid URI, and it could not be converted to a file URI: " << error->message;
			g_error_free(error);
			return -1;
		}
	}


	qDebug() << "Playing media from URL:" << inputUrl;
	qDebug() << "Running in fullscreen:" << runInFullscreen;


	// Create a dummy QML GL sink element to force the
	// Gst Qt plugin to initialize and register the
	// GstGLVideoItem QML element. (Subsequent instantiations
	// of qmlglsink will not repeat this registration.)
	// Do this _before_ loading the QML interface.
	GstElement *dummy_qmlglsink = gst_element_factory_make("qmlglsink", nullptr);
	if (dummy_qmlglsink == nullptr)
	{
		qCritical() << "Could not create qmlglsink";
		return -1;
	}
	else
		gst_object_unref(GST_OBJECT(dummy_qmlglsink));


	QQmlApplicationEngine qml_engine;
	qml_engine.load(QUrl("qrc:/main.qml"));
	if (qml_engine.rootObjects().empty())
	{
		qCritical() << "Could not get user interface QML script";
		return -1;
	}


	// Get the main QML user interface window. The window is the
	// root object in the QML user interface hierarchy.
	QQuickWindow *mainWindow = qobject_cast<QQuickWindow*>(qml_engine.rootObjects().value(0));


	Pipeline pipeline;
	if (!pipeline.setup(inputUrl, mainWindow))
		return -1;

	// Install the signal handlers. They will call the main window's
	// quit() application when these handlers catch a signal.
	if (!sighandler.setup(mainWindow))
		return -1;

	// Show the window, fullscreen if requested.
	if (runInFullscreen)
		mainWindow->showFullScreen();
	else
		mainWindow->show();


	// Get the GLVideoItem from the QML user interface.

	QQuickItem *videoItem = mainWindow->findChild<QQuickItem *>("videoItem");
	if (videoItem == nullptr)
	{
		qCritical() << "Could not find video item";
		return -1;
	}


	// NOTE: On Wayland and X11, both of these approaches work.
	// On EGLFS however, only the second renderjob based approach
	// works. It is currently unknown why this is the case.
#if 0
	// Connect to sceneGraphInitialized to start the pipeline as soon as the
	// QML interface is up and running. Trying to start the pipeline earlier
	// produces errors because qmlglsink can't get the EGL context.
	QObject::connect(mainWindow, &QQuickWindow::sceneGraphInitialized, [&]() {
		qDebug() << "Starting pipeline";

		if (!pipeline.start(videoItem))
		{
			qCritical() << "Could not start pipeline; quitting";
			app.quit();
		}
	});
#else
	// Create an instance of the SetPlayingJob helper class and schedule it
	// to be run as a render job. This is a trick to ensure that the pipeline
	// start code that is located inside SetPlayingJob::run() is executed
	// _after_ the scenegraph is up and running, implying that the EGL context
	// is initialized and valid (this is required by qmlglsink).
	mainWindow->scheduleRenderJob(
		new SetPlayingJob(pipeline, videoItem, app),
		QQuickWindow::BeforeSynchronizingStage
	);
#endif


	return app.exec();
}
