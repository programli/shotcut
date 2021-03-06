/*
 * Copyright (c) 2020 Meltytech, LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "proxymanager.h"
#include "mltcontroller.h"
#include "settings.h"
#include "shotcut_mlt_properties.h"
#include "jobqueue.h"
#include "jobs/ffmpegjob.h"
#include "jobs/meltjob.h"
#include "util.h"

#include <QObject>
#include <QVector>
#include <QTemporaryFile>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <Logger.h>

static const char* kProxyVideoExtension = ".mp4";
static const char* kProxyPendingVideoExtension = ".pending.mp4";
static const char* kProxyImageExtension = ".jpg";
static const char* kProxyPendingImageExtension = ".pending.jpg";
static const float kProxyResolutionRatio = 1.3f;
static const int   kFallbackProxyResolution = 540;

static bool isValidImage(Mlt::Producer& producer)
{
    QString service = QString::fromLatin1(producer.get("mlt_service"));
    return (service == "qimage" || service == "pixbuf") && !producer.get_int(kShotcutSequenceProperty);
}

QDir ProxyManager::dir()
{
    // Use project folder + "/proxies" if using project folder and enabled
    QDir dir(MLT.projectFolder());
    if (!MLT.projectFolder().isEmpty() && dir.exists() && Settings.proxyUseProjectFolder()) {
        const char* subfolder = "proxies";
        if (!dir.cd(subfolder)) {
            if (dir.mkdir(subfolder))
                dir.cd(subfolder);
        }
    } else {
        // Otherwise, use app setting
        dir = QDir(Settings.proxyFolder());
    }
    return dir;
}

QString ProxyManager::resource(Mlt::Service& producer)
{
    QString resource = QString::fromUtf8(producer.get("resource"));
    if (producer.get_int(kIsProxyProperty) && producer.get(kOriginalResourceProperty)) {
        resource = QString::fromUtf8(producer.get(kOriginalResourceProperty));
    } else if (!::qstrcmp(producer.get("mlt_service"), "timewarp")) {
        resource = QString::fromUtf8(producer.get("warp_resource"));
    }
    return resource;
}

void ProxyManager::generateVideoProxy(Mlt::Producer& producer, bool fullRange, ScanMode scanMode, const QPoint& aspectRatio, bool replace)
{
    // Always regenerate per preview scaling or 540 if not specified
    QString resource = ProxyManager::resource(producer);
    QStringList args;
    QString hash = Util::getHash(producer);
    QString fileName = ProxyManager::dir().filePath(hash + kProxyPendingVideoExtension);
    QString filters;
    auto hwCodecs = Settings.encodeHardware();
    QString hwFilters;

    // Touch file to make it in progress
    QFile file(fileName);
    file.open(QIODevice::WriteOnly);
    file.resize(0);
    file.close();

    args << "-loglevel" << "verbose";
    args << "-i" << resource;
    args << "-max_muxing_queue_size" << "9999";
    // transcode all streams except data, subtitles, and attachments
    if (producer.get_int("video_index") < producer.get_int("audio_index"))
        args << "-map" << "0:v?" << "-map" << "0:a?";
    else
        args << "-map" << "0:a?" << "-map" << "0:v?";
    args << "-map_metadata" << "0" << "-ignore_unknown";
    args << "-vf";

    if (scanMode == Automatic) {
        filters = QString("yadif=deint=interlaced,");
    } else if (scanMode != Progressive) {
        filters = QString("yadif=parity=%1,").arg(scanMode == InterlacedTopFieldFirst? "tff" : "bff");
    }
    filters += QString("scale=width=-2:height=%1").arg(resolution());
    if (Settings.proxyUseHardware() && (hwCodecs.contains("hevc_vaapi") || hwCodecs.contains("h264_vaapi"))) {
        hwFilters = ",format=nv12,hwupload";
    }
    if (fullRange) {
        args << filters + ":in_range=full:out_range=full" + hwFilters;
        args << "-color_range" << "jpeg";
    } else {
        args << filters + ":in_range=mpeg:out_range=mpeg" + hwFilters;
        args << "-color_range" << "mpeg";
    }
    switch (producer.get_int("meta.media.colorspace")) {
    case 601:
        if (producer.get_int("meta.media.height") == 576) {
            args << "-color_primaries" << "bt470bg";
            args << "-color_trc" << "smpte170m";
            args << "-colorspace" << "bt470bg";
        } else {
            args << "-color_primaries" << "smpte170m";
            args << "-color_trc" << "smpte170m";
            args << "-colorspace" << "smpte170m";
        }
        break;
    case 170:
        args << "-color_primaries" << "smpte170m";
        args << "-color_trc" << "smpte170m";
        args << "-colorspace" << "smpte170m";
        break;
    case 240:
        args << "-color_primaries" << "smpte240m";
        args << "-color_trc" << "smpte240m";
        args << "-colorspace" << "smpte240m";
        break;
    case 470:
        args << "-color_primaries" << "bt470bg";
        args << "-color_trc" << "bt470bg";
        args << "-colorspace" << "bt470bg";
        break;
    default:
        args << "-color_primaries" << "bt709";
        args << "-color_trc" << "bt709";
        args << "-colorspace" << "bt709";
        break;
    }
    if (!aspectRatio.isNull()) {
        args << "-aspect" << QString("%1:%2").arg(aspectRatio.x()).arg(aspectRatio.y());
    }
    args << "-f" << "mp4" << "-codec:a" << "ac3" << "-b:a" << "256k";
    args << "-pix_fmt" << "yuv420p";
    if (Settings.proxyUseHardware()) {
        if (hwCodecs.contains("hevc_nvenc")) {
            args << "-codec:v" << "hevc_nvenc";
            args << "-rc" << "constqp";
            args << "-vglobal_quality" << "37";
        } else if (hwCodecs.contains("hevc_qsv")) {
            args << "-load_plugin" << "hevc_hw";
            args << "-codec:v" << "hevc_qsv";
            args << "-global_quality:v" << "36";
            args << "-look_ahead" << "1";
        } else if (hwCodecs.contains("hevc_amf")) {
            args << "-codec:v" << "hevc_amf";
            args << "-rc" << "1";
            args << "-qp_i" << "32" << "-qp_p" << "32";
        } else if (hwCodecs.contains("hevc_vaapi")) {
            args << "-init_hw_device" << "vaapi=vaapi0:,connection_type=x11" << "-filter_hw_device" << "vaapi0";
            args << "-codec:v" << "hevc_vaapi";
            args << "-qp" << "37";
        } else if (hwCodecs.contains("h264_vaapi")) {
            args << "-init_hw_device" << "vaapi=vaapi0:,connection_type=x11" << "-filter_hw_device" << "vaapi0";
            args << "-codec:v" << "h264_vaapi";
            args << "-qp" << "30";
        } else if (hwCodecs.contains("hevc_videotoolbox")) {
            args << "-codec:v" << "hevc_videotoolbox";
            args << "-b:v" << "2M";
        }
    }
    if (!args.contains("-codec:v")) {
        args << "-codec:v" << "libx264";
        args << "-preset" << "veryfast";
        args << "-crf" << "23";
    }
    args << "-g" << "1" << "-bf" << "0";
    args << "-y" << fileName;

    FfmpegJob* job = new FfmpegJob(fileName, args, false);
    job->setLabel(QObject::tr("Make proxy for %1").arg(Util::baseName(resource)));
    if (replace) {
        job->setPostJobAction(new ProxyReplacePostJobAction(resource, fileName, hash));
    } else {
        job->setPostJobAction(new ProxyFinalizePostJobAction(fileName));
    }
    JOBS.add(job);
}

void ProxyManager::generateImageProxy(Mlt::Producer& producer, bool replace)
{
    // Always regenerate per preview scaling or 540 if not specified
    QString resource = ProxyManager::resource(producer);
    QStringList args;
    QString hash = Util::getHash(producer);
    QString fileName = ProxyManager::dir().filePath(hash + kProxyPendingImageExtension);
    QString filters;

    // Touch file to make it in progress
    QFile file(fileName);
    file.open(QIODevice::WriteOnly);
    file.resize(0);
    file.close();

    auto width = producer.get_double("meta.media.width");
    auto height = producer.get_double("meta.media.height");
    args << "-verbose" << "-profile" << "square_pal";
    args << resource << "out=0" << "-consumer";
    args << QString("avformat:%1").arg(fileName);
    args << QString("width=%1").arg(qRound(width / height * resolution()));
    args << QString("height=%1").arg(resolution());
    args << "pix_fmt=yuvj422p" << "color_range=full";

    MeltJob* job = new MeltJob(fileName, args, 1, 1);
    job->setLabel(QObject::tr("Make proxy for %1").arg(Util::baseName(resource)));
    if (replace) {
        job->setPostJobAction(new ProxyReplacePostJobAction(resource, fileName, hash));
    } else {
        job->setPostJobAction(new ProxyFinalizePostJobAction(fileName));
    }
    JOBS.add(job);
}

typedef QPair<QString, QString> MltProperty;

static void processProperties(QXmlStreamWriter& newXml, QVector<MltProperty>& properties, const QString& root)
{
    // Determine if this is a proxy resource
    bool isProxy = false;
    QString newResource;
    QString service;
    QString speed = "1";
    for (const auto& p: properties) {
        if (p.first == kIsProxyProperty) {
            isProxy = true;
        } else if (p.first == kOriginalResourceProperty) {
            newResource = p.second;
        } else if (newResource.isEmpty() && p.first == "resource") {
            newResource = p.second;
        } else if (p.first == "mlt_service") {
            service = p.second;
        } else if (p.first == "warp_speed") {
            speed = p.second;
        }
    }
    QVector<MltProperty> newProperties;
    QVector<MltProperty>& propertiesRef = properties;
    if (isProxy) {
        // Filter the properties
        for (const auto& p: properties) {
            // Replace the resource property if proxy
            if (p.first == "resource") {
                // Convert to relative
                if (!root.isEmpty() && newResource.startsWith(root)) {
                    if (root.endsWith('/'))
                        newResource = newResource.mid(root.size());
                    else
                        newResource = newResource.mid(root.size() + 1);
                }
                if (service == "timewarp") {
                    newProperties << MltProperty(p.first, QString("%1:%2").arg(speed).arg(newResource));
                } else {
                    newProperties << MltProperty(p.first, newResource);
                }
            } else if (p.first == "warp_resource") {
                newProperties << MltProperty(p.first, newResource);
            // Remove special proxy and original resource properties
            } else if (p.first != kIsProxyProperty && p.first != kOriginalResourceProperty) {
                newProperties << MltProperty(p.first, p.second);
            }
        }
        propertiesRef = newProperties;
    }
    // Write all of the property elements
    for (const auto& p : propertiesRef) {
        newXml.writeStartElement("property");
        newXml.writeAttribute("name", p.first);
        newXml.writeCharacters(p.second);
        newXml.writeEndElement();
    }
    // Reset the saved properties
    properties.clear();
}

bool ProxyManager::filterXML(QString& fileName, const QString& root)
{
    QFile file(fileName);
    QTemporaryFile tempFile(QFileInfo(fileName).dir().filePath("shotcut-XXXXXX.mlt"));
    if (file.open(QIODevice::ReadOnly | QIODevice::Text) && tempFile.open()) {
        tempFile.resize(0);
        QXmlStreamReader xml(&file);
        QXmlStreamWriter newXml(&tempFile);
        bool isPropertyElement = false;
        QVector<MltProperty> properties;

        newXml.setAutoFormatting(true);
        newXml.setAutoFormattingIndent(2);

        while (!xml.atEnd()) {
            switch (xml.readNext()) {
            case QXmlStreamReader::Characters:
                if (!isPropertyElement)
                    newXml.writeCharacters(xml.text().toString());
                break;
            case QXmlStreamReader::Comment:
                newXml.writeComment(xml.text().toString());
                break;
            case QXmlStreamReader::DTD:
                newXml.writeDTD(xml.text().toString());
                break;
            case QXmlStreamReader::EntityReference:
                newXml.writeEntityReference(xml.name().toString());
                break;
            case QXmlStreamReader::ProcessingInstruction:
                newXml.writeProcessingInstruction(xml.processingInstructionTarget().toString(), xml.processingInstructionData().toString());
                break;
            case QXmlStreamReader::StartDocument:
                newXml.writeStartDocument(xml.documentVersion().toString(), xml.isStandaloneDocument());
                break;
            case QXmlStreamReader::EndDocument:
                newXml.writeEndDocument();
                break;
            case QXmlStreamReader::StartElement: {
                const QString element = xml.name().toString();
                if (element == "property") {
                    // Save each property element but do not output yet
                    const QString name = xml.attributes().value("name").toString();
                    properties << MltProperty(name, xml.readElementText());
                    isPropertyElement = true;
                } else {
                    // At the start of a non-property element
                    isPropertyElement = false;
                    processProperties(newXml, properties, root);
                    // Write the new start element
                    newXml.writeStartElement(xml.namespaceUri().toString(), element);
                    for (const auto& a : xml.attributes()) {
                        newXml.writeAttribute(a);
                    }
                }
                break;
            }
            case QXmlStreamReader::EndElement:
                // At the end of a non-property element
                if (xml.name() != "property") {
                    processProperties(newXml, properties, root);
                    newXml.writeEndElement();
                }
                break;
            default:
                break;
            }
        }
        if (tempFile.isOpen())
            tempFile.close();

        // Useful for debugging
//        tempFile.open();
//        LOG_DEBUG() << tempFile.readAll().constData();
//        tempFile.close();

        if (!xml.hasError()) {
            fileName = tempFile.fileName();
            LOG_DEBUG() << fileName;
            tempFile.setAutoRemove(false);
            return true;
        }
    }
    return false;
}

bool ProxyManager::fileExists(Mlt::Producer& producer)
{
    QDir proxyDir(Settings.proxyFolder());
    QDir projectDir(MLT.projectFolder());
    QString service = QString::fromLatin1(producer.get("mlt_service"));
    QString fileName;
    if (service.startsWith("avformat")) {
        fileName = Util::getHash(producer) + kProxyVideoExtension;
    } else if (isValidImage(producer)) {
        fileName = Util::getHash(producer) + kProxyImageExtension;
    } else {
        return false;
    }
    return (projectDir.cd("proxies") && projectDir.exists(fileName)) || proxyDir.exists(fileName);
}

bool ProxyManager::filePending(Mlt::Producer& producer)
{
    QDir proxyDir(Settings.proxyFolder());
    QDir projectDir(MLT.projectFolder());
    QString service = QString::fromLatin1(producer.get("mlt_service"));
    QString fileName;
    if (service.startsWith("avformat")) {
        fileName = Util::getHash(producer) + kProxyPendingVideoExtension;
    } else if (isValidImage(producer)) {
        fileName = Util::getHash(producer) + kProxyPendingImageExtension;
    } else {
        return false;
    }
    return (projectDir.cd("proxies") && projectDir.exists(fileName)) || proxyDir.exists(fileName);
}

// Returns true if the producer exists and was updated with proxy info
bool ProxyManager::generateIfNotExists(Mlt::Producer& producer, bool replace)
{
    if (Settings.proxyEnabled() && producer.is_valid() && !producer.get_int(kDisableProxyProperty) && !producer.get_int(kIsProxyProperty)) {
        QString service = QString::fromLatin1(producer.get("mlt_service"));
        if (ProxyManager::fileExists(producer)) {
            QDir proxyDir(Settings.proxyFolder());
            QDir projectDir(MLT.projectFolder());
            QString fileName;
            if (service.startsWith("avformat")) {
                fileName = Util::getHash(producer) + kProxyVideoExtension;
            } else if (isValidImage(producer)) {
                fileName = Util::getHash(producer) + kProxyImageExtension;
            } else {
                return false;
            }
            producer.set(kIsProxyProperty, 1);
            producer.set(kOriginalResourceProperty, producer.get("resource"));
            if (projectDir.exists(fileName)) {
                producer.set("resource", projectDir.filePath(fileName).toUtf8().constData());
            } else {
                producer.set("resource", proxyDir.filePath(fileName).toUtf8().constData());
            }
            return true;
        } else if (!filePending(producer)) {
            if (service.startsWith("avformat")) {
                // Tag this producer so we do not try to generate proxy again in this session
                delete producer.get_frame();
                auto threshold = qRound(kProxyResolutionRatio * resolution());
                LOG_DEBUG() << producer.get_int("meta.media.width") << "x" << producer.get_int("meta.media.height") << "threshold" << threshold;
                if (producer.get_int("meta.media.width") > threshold && producer.get_int("meta.media.height") > threshold) {
                    ProxyManager::generateVideoProxy(producer, MLT.fullRange(producer), Automatic, QPoint(), replace);
                }
            } else if (isValidImage(producer)) {
                // Tag this producer so we do not try to generate proxy again in this session
                delete producer.get_frame();
                auto threshold = qRound(kProxyResolutionRatio * resolution());
                LOG_DEBUG() << producer.get_int("meta.media.width") << "x" << producer.get_int("meta.media.height") << "threshold" << threshold;
                if (producer.get_int("meta.media.width") > threshold && producer.get_int("meta.media.height") > threshold) {
                    ProxyManager::generateImageProxy(producer, replace);
                }
            }
        }
    }
    return false;
}

const char* ProxyManager::videoFilenameExtension()
{
    return kProxyVideoExtension;
}

const char* ProxyManager::pendingVideoExtension()
{
    return kProxyPendingVideoExtension;
}

const char* ProxyManager::imageFilenameExtension()
{
    return kProxyImageExtension;
}

const char* ProxyManager::pendingImageExtension()
{
    return kProxyImageExtension;
}

int ProxyManager::resolution()
{
    return Settings.playerPreviewScale()? Settings.playerPreviewScale() : kFallbackProxyResolution;
}

class FindNonProxyProducersParser : public Mlt::Parser
{
private:
    QString m_hash;
    QList<Mlt::Producer> m_producers;

public:
    FindNonProxyProducersParser() : Mlt::Parser() {}

    QList<Mlt::Producer>& producers() { return m_producers; }

    int on_start_filter(Mlt::Filter*) { return 0; }
    int on_start_producer(Mlt::Producer* producer) {
        if (!producer->parent().get_int(kIsProxyProperty))
            m_producers << Mlt::Producer(producer);
        return 0;
    }
    int on_end_producer(Mlt::Producer*) { return 0; }
    int on_start_playlist(Mlt::Playlist*) { return 0; }
    int on_end_playlist(Mlt::Playlist*) { return 0; }
    int on_start_tractor(Mlt::Tractor*) { return 0; }
    int on_end_tractor(Mlt::Tractor*) { return 0; }
    int on_start_multitrack(Mlt::Multitrack*) { return 0; }
    int on_end_multitrack(Mlt::Multitrack*) { return 0; }
    int on_start_track() { return 0; }
    int on_end_track() { return 0; }
    int on_end_filter(Mlt::Filter*) { return 0; }
    int on_start_transition(Mlt::Transition*) { return 0; }
    int on_end_transition(Mlt::Transition*) { return 0; }
};

void ProxyManager::generateIfNotExistsAll(Mlt::Producer& producer)
{
    FindNonProxyProducersParser parser;
    parser.start(producer);
    for (auto& clip : parser.producers()) {
        generateIfNotExists(clip, false /* replace */);
        clip.set(kIsProxyProperty, 1);
    }
}
