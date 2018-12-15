#include "epubcontainer.h"

#include <quazip5/quazip.h>
#include <quazip5/quazipfile.h>

#include <QDebug>
#include <QDir>
#include <QDomDocument>
#include <QImage>
#include <QImageReader>
#include <QScopedPointer>
#include <QSharedPointer>

#include <csvsplitter/csvsplitter.h>
#include <qlogger/qlogger.h>

#include <ebookcommon.h>

using namespace qlogger;

const QString EPubContainer::METADATA_FOLDER = "META-INF";
const QString EPubContainer::MIMETYPE_FILE = "mimetype";
const QString EPubContainer::CONTAINER_FILE = "META-INF/container.xml";
const QString EPubContainer::TOC_FILE = "toc.ncx";

EPubContainer::EPubContainer(QObject* parent)
    : QObject(parent)
    , m_archive(Q_NULLPTR)
    , m_toc(new EPubToc())
{}

EPubContainer::~EPubContainer() {}

bool
EPubContainer::openFile(const QString path) {

    m_archive = new QuaZip(path);
    m_filename = path; // stored against modification;

    if (!m_archive->open(QuaZip::mdUnzip)) {
        emit errorHappened(tr("Failed to open %1").arg(path));
        return false;
    }

    QStringList files = m_archive->getFileNameList();
    if (files.isEmpty()) {
        emit errorHappened(tr("Failed to read %1").arg(path));
        return false;
    }

    if (!parseMimetype(files)) {
        return false;
    }

    if (!parseContainer(files)) {
        return false;
    }

    if (!parseToc(files)) {
        return false;
    }

    return true;
}

QSharedPointer<QuaZipFile>
EPubContainer::zipFile(const QString& path) {
    m_archive->setCurrentFile(path);
    QuaZipFile* file = new QuaZipFile(path);
    file->setZip(m_archive);

    if (!file->open(QIODevice::ReadOnly)) {
        emit errorHappened(tr("Unable to open file %1").arg(path));
        return QSharedPointer<QuaZipFile>();
    }

    return QSharedPointer<QuaZipFile>(file);
}

QImage
EPubContainer::image(const QString& id) {
    if (!m_items.contains(id)) {
        QLOG_WARN(tr("Asked for unknown item %1").arg(id));
        return QImage();
    }

    const EPubItem& item = m_items.value(id);

    if (!QImageReader::supportedMimeTypes().contains(item.mimetype)) {
        QLOG_WARN(tr("Asked for unsupported type %1").arg(QString(item.mimetype)));
        return QImage();
    }

    QSharedPointer<QuaZipFile> unzippedFile = zipFile(item.path);

    if (!unzippedFile) {
        return QImage();
    }

    return QImage::fromData(unzippedFile->readAll());
}

QString
EPubContainer::metadata(const QString& key) {
    return m_metadata.value(key);
}

void
EPubContainer::setMetadata(const QString& key, const QString& value) {
    m_metadata[key] = value;
}

ebooktoc_t
EPubContainer::toc() {
    return m_toc;
}

bool
EPubContainer::parseMimetype(QStringList& files) {
    if (files.contains(MIMETYPE_FILE)) {
        m_archive->setCurrentFile(MIMETYPE_FILE);
        QuaZipFile mimetypeFile(m_archive);

        if (!mimetypeFile.open(QIODevice::ReadOnly)) {
            m_archive->getZipError();
            emit errorHappened(tr("Unable to open mimetype file"));
            return false;
        }

        QByteArray mimetype = mimetypeFile.readAll();
        if (mimetype != "application/epub+zip") {
            emit errorHappened(tr("Unexpected mimetype %1").arg(QString(mimetype)));
        }
    } else {
        emit errorHappened(tr("Unable to find mimetype in file"));
        return false;
    }

    return true;
}

bool
EPubContainer::parseContainer(QStringList& files) {
    if (files.contains(CONTAINER_FILE)) {
        m_archive->setCurrentFile(CONTAINER_FILE);
        QuaZipFile containerFile(m_archive);
        containerFile.setZip(m_archive);

        if (!containerFile.open(QIODevice::ReadOnly)) {
            int error = m_archive->getZipError();
            emit errorHappened(
                tr("Unable to open container file error %1").arg(error));
            return false;
        }

        QString container(containerFile.readAll());

        QDomDocument document;
        document.setContent(container);
        QDomNodeList rootNodes = document.elementsByTagName("rootfile");
        for (int i = 0; i < rootNodes.count(); i++) {
            QDomElement rootElement = rootNodes.at(i).toElement();
            QString rootfilePath = rootElement.attribute("full-path");
            if (rootfilePath.isEmpty()) {
                QLOG_WARN(tr("Invalid root file entry"));
                continue;
            }
            if (parseContentFile(rootfilePath)) {
                return true;
            }
        }

    } else {
        emit errorHappened(tr("Unable to find container information"));
        return false;
    }

    // Limitations:
    //  - We only read one rootfile
    //  - We don't read the following from META-INF/
    //     - manifest.xml (unknown contents, just reserved)
    //     - metadata.xml (unused according to spec, just reserved)
    //     - rights.xml (reserved for DRM, not standardized)
    //     - signatures.xml (signatures for files, standardized)

    emit errorHappened(tr("Unable to find and use any content files"));
    return false;
}

bool
EPubContainer::parseContentFile(const QString filepath) {
    m_archive->setCurrentFile(filepath);
    QuaZipFile contentFile(m_archive);
    contentFile.setZip(m_archive);

    if (!contentFile.open(QIODevice::ReadOnly)) {
        emit errorHappened(
            tr("Malformed metadata, unable to get content metadata path"));
        return false;
    }

    QString content(contentFile.readAll());
    QDomDocument document;
    document.setContent(content, true); // turn on namespace processing

    // parse metedata.
    QDomNodeList metadataNodeList = document.elementsByTagName("metadata");
    for (int i = 0; i < metadataNodeList.count(); i++) {
        QDomNodeList metadataChildList = metadataNodeList.at(i).childNodes();
        for (int j = 0; j < metadataChildList.count(); j++) {
            parseMetadataItem(metadataChildList.at(j));
        }
    }

    // Extract current path, for resolving relative paths
    QString contentFileFolder;
    int separatorIndex = filepath.lastIndexOf('/');
    if (separatorIndex > 0) {
        contentFileFolder = filepath.left(separatorIndex + 1);
    }

    // Parse out all the components/items in the epub
    QDomNodeList manifestNodeList = document.elementsByTagName("manifest");
    for (int i = 0; i < manifestNodeList.count(); i++) {
        QDomElement manifestElement = manifestNodeList.at(i).toElement();
        QDomNodeList manifestItemList = manifestElement.elementsByTagName("item");

        for (int j = 0; j < manifestItemList.count(); j++) {
            parseManifestItem(manifestItemList.at(j), contentFileFolder);
        }
    }

    // Parse out the document order
    QDomNodeList spineNodeList = document.elementsByTagName("spine");
    for (int i = 0; i < spineNodeList.count(); i++) {
        QDomElement spineElement = spineNodeList.at(i).toElement();

        QString tocId = spineElement.attribute("toc");
        if (!tocId.isEmpty() && m_items.keys().contains(tocId)) {
            EPubPageReference tocReference;
            tocReference.title = tr("Table of Contents");
            tocReference.target = tocId;
            m_standardReferences.insert(EPubPageReference::TableOfContents,
                                        tocReference);
        }

        QDomNodeList spineItemList = spineElement.elementsByTagName("itemref");
        for (int j = 0; j < spineItemList.count(); j++) {
            parseSpineItem(spineItemList.at(j));
        }
    }

    // Parse out standard items
    QDomNodeList guideNodeList = document.elementsByTagName("guide");
    for (int i = 0; i < guideNodeList.count(); i++) {
        QDomElement guideElement = guideNodeList.at(i).toElement();

        QDomNodeList guideItemList = guideElement.elementsByTagName("reference");
        for (int j = 0; j < guideItemList.count(); j++) {
            parseGuideItem(guideItemList.at(j));
        }
    }

    return true;
}

bool
EPubContainer::parseToc(QStringList& files) {
    if (files.contains(TOC_FILE)) {
        m_archive->setCurrentFile(TOC_FILE);
        QuaZipFile containerFile(m_archive);
        containerFile.setZip(m_archive);

        if (!containerFile.open(QIODevice::ReadOnly)) {
            int error = m_archive->getZipError();
            emit errorHappened(
                tr("Unable to open container file error %1").arg(error));
            return false;
        }

        QString data = containerFile.readAll();

        bool ok;
        QDomDocument document;
        if (document.setContent(data)) {
            QDomElement rootNode = document.documentElement();
            m_toc->version = rootNode.attribute("version");
            m_toc->xmlns = rootNode.attribute("xmlns");
            m_toc->xml_lang = rootNode.attribute("xml:lang");

            QDomElement head = rootNode.firstChildElement("head");
            QDomElement meta = head.firstChildElement("meta");
            while (!meta.isNull()) {
                QString name = meta.attribute("name");
                QString content = meta.attribute("content");
                m_toc->metadata[name] = content;
                meta = head.nextSiblingElement("navPoint");
            }

            QDomElement titleNode = rootNode.firstChildElement("docTitle");
            m_toc->title = titleNode.text();

            QDomElement navMap = rootNode.firstChildElement("navMap");
            QDomElement navPoint = navMap.firstChildElement("navPoint");
            while (!navPoint.isNull()) {
                QString classname = navPoint.attribute("class");
                QString id = navPoint.attribute("id");
                int playorder = navPoint.attribute("playOrder").toInt(&ok);

                QDomElement navLabel = navPoint.firstChildElement("navLabel");
                QString label = navLabel.text();

                QDomElement content = navPoint.firstChildElement("content");
                QString src = content.attribute("src");

                navpoint_t navpoint(new EPubNavPoint(classname, id, label, src));
                m_toc->navmap[playorder] = navpoint;

                navPoint = navPoint.nextSiblingElement("navPoint");
            }
        }
    }
    return true;
}

bool
EPubContainer::writeToc() {
    // TODO write TOC file.
    return false;
}

bool
EPubContainer::parseMetadataItem(const QDomNode& metadataNode) {
    QDomElement metadataElement = metadataNode.toElement();
    QString tagName = metadataElement.tagName();
    QDomNamedNodeMap nodeMap = metadataElement.attributes();

    QString metaName;
    QString metaValue;

    if (tagName == "meta") {
        metaName = metadataElement.attribute("name");
        metaValue = metadataElement.attribute("content");
    } else if (metadataElement.prefix() != "dc") {
        QLOG_WARN(tr("Unsupported metadata tag %1").arg(tagName));
        return false;
    } else if (tagName == "date") {
        metaName = metadataElement.attribute("event");
        metaValue = metadataElement.text();
    } else if (tagName == "creator") {
        metaName = tagName;
        if (m_metadata["creator"].isEmpty()) {
            metaValue = metadataElement.text();
        } else {
            metaValue = m_metadata["creator"];
            metaValue += "; ";
            metaValue += metadataElement.text();
        }
    } else {
        metaName = tagName;
        metaValue = metadataElement.text();
    }

    if (metaName.isEmpty() || metaValue.isEmpty()) {
        return false;
    }
    m_metadata[metaName] = metaValue;

    if (!nodeMap.isEmpty()) {
        QMap<QString, QString> map;
        for (int i = 0; i < nodeMap.count(); i++) {
            QDomNode node = nodeMap.item(i);
            QString name = node.nodeName();
            QString value = node.nodeValue();
            map[name] = value;
        }
        m_othermetatags[metaName] = map;
    }

    return true;
}

bool
EPubContainer::parseManifestItem(const QDomNode& manifestNode,
                                 const QString currentFolder) {
    QDomElement manifestElement = manifestNode.toElement();
    QString id = manifestElement.attribute("id");
    QString path = manifestElement.attribute("href");
    QString type = manifestElement.attribute("media-type");

    if (id.isEmpty() || path.isEmpty()) {
        QLOG_WARN(tr("Invalid item at line %1").arg(manifestElement.lineNumber()));
        return false;
    }

    // Resolve relative paths
    path = QDir::cleanPath(currentFolder + path);

    EPubItem item;
    item.mimetype = type.toUtf8();
    item.path = path;
    m_items[id] = item;

    static QSet<QString> documentTypes({ "text/x-oeb1-document",
                                         "application/x-dtbook+xml",
                                         "application/xhtml+xml" });
    // All items not listed in the spine should be in this
    if (documentTypes.contains(type)) {
        m_unorderedItems.insert(id);
    }

    return true;
}

bool
EPubContainer::parseSpineItem(const QDomNode& spineNode) {
    QDomElement spineElement = spineNode.toElement();

    // Ignore this for now
    if (spineElement.attribute("linear") == "no") {
        //        return true;
    }

    QString referenceName = spineElement.attribute("idref");
    if (referenceName.isEmpty()) {
        QLOG_WARN(tr("Invalid spine item at line %1").arg(spineNode.lineNumber()));
        return false;
    }

    if (!m_items.keys().contains(referenceName)) {
        QLOG_WARN(tr("Unable to find %1 in items").arg(referenceName));
        return false;
    }

    m_unorderedItems.remove(referenceName);
    m_orderedItems.append(referenceName);

    return true;
}

bool
EPubContainer::parseGuideItem(const QDomNode& guideItem) {
    QDomElement guideElement = guideItem.toElement();
    QString target = guideElement.attribute("href");
    QString title = guideElement.attribute("title");
    QString type = guideElement.attribute("type");

    if (target.isEmpty() || title.isEmpty() || type.isEmpty()) {
        QLOG_WARN(
            tr("Invalid guide item %1 %2 %3").arg(target).arg(title).arg(type));
        return false;
    }

    EPubPageReference reference;
    reference.target = target;
    reference.title = title;

    EPubPageReference::StandardType standardType =
        EPubPageReference::typeFromString(type);
    if (standardType == EPubPageReference::Other) {
        m_otherReferences[type] = reference;
    } else {
        m_standardReferences[standardType] = reference;
    }

    return true;
}

// const QuaZip *EPubContainer:::getFile(const QString &path) {
//  if (path.isEmpty()) {
//    return Q_NULLPTR;
//  }

//  const KArchiveDirectory *folder = m_rootFolder;

//  // Try to walk down the correct path
//  QStringList pathParts = path.split('/', QString::SkipEmptyParts);
//  for (int i = 0; i < pathParts.count() - 1; i++) {
//    QString folderName = pathParts[i];
//    const KArchiveEntry *entry = folder->entry(folderName);
//    if (!entry) {
//      qWarning() << "Unable to find folder name" << folderName << "in" <<
//      path; return nullptr;
//    }
//    if (!entry->isDirectory()) {
//      qWarning() << "Expected" << folderName << "to be a directory in path"
//                 << path;
//      return nullptr;
//    }

//    folder = dynamic_cast<const KArchiveDirectory *>(entry);
//    Q_ASSERT(folder);
//  }

//  QString filename;
//  if (pathParts.isEmpty()) {
//    filename = path;
//  } else {
//    filename = pathParts.last();
//  }

//  const KArchiveFile *file = folder->file(filename);
//  if (!file) {
//    qWarning() << "Unable to find file" << filename << "in" <<
//    folder->name();
//  }
//  return file;
//}

EPubPageReference::StandardType
EPubPageReference::typeFromString(const QString& name) {
    if (name == "cover") {
        return CoverPage;
    } else if (name == "title-page") {
        return TitlePage;
    } else if (name == "toc") {
        return TableOfContents;
    } else if (name == "index") {
        return Index;
    } else if (name == "glossary") {
        return Glossary;
    } else if (name == "acknowledgements") {
        return Acknowledgements;
    } else if (name == "bibliography") {
        return Bibliography;
    } else if (name == "colophon") {
        return Colophon;
    } else if (name == "copyright-page") {
        return CopyrightPage;
    } else if (name == "dedication") {
        return Dedication;
    } else if (name == "epigraph") {
        return Epigraph;
    } else if (name == "foreword") {
        return Foreword;
    } else if (name == "loi") {
        return ListOfIllustrations;
    } else if (name == "lot") {
        return ListOfTables;
    } else if (name == "notes") {
        return Notes;
    } else if (name == "preface") {
        return Preface;
    } else if (name == "text") {
        return Text;
    } else {
        return Other;
    }
}
