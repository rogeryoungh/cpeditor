/*
 * Copyright (C) 2019-2021 Ashar Khan <ashar786khan@gmail.com>
 *
 * This file is part of CP Editor.
 *
 * CP Editor is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * I will not be responsible if CP Editor behaves in unexpected way and
 * causes your ratings to go down and or lose any important contest.
 *
 * Believe Software is "Software" and it isn't immune to bugs.
 *
 */
#include "LanguageServer.hpp"
#include "Core/EventLogger.hpp"
#include "Core/MessageLogger.hpp"
#include "Extensions/LSPCompleter.hpp"
#include "Settings/SettingsManager.hpp"
#include "Util/Util.hpp"
#include "third_party/lsp-cpp/include/LSPClient.hpp"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

#include <QDebug>

namespace Extensions
{

LanguageServer::LanguageServer(QString const &lang)
{
    LOG_INFO(INFO_OF(lang));
    this->language = lang;
    if (shouldCreateClient())
    {
        createClient();
        performConnection();
    }
}

LanguageServer::~LanguageServer()
{
    if (lsp != nullptr)
    {
        LOG_INFO("Killing LSP");
        lsp->shutdown();
        lsp->exit();
        delete lsp;
    }
}

void LanguageServer::openDocument(QString const &path, QCodeEditor *editor, MessageLogger *log)
{
    if (isDocumentOpen())
    {
        LOG_WARN("openDocument called without closing the previous document. Closing it now");
        closeDocument();
    }

    m_editor = editor;
    openFile = path;
    logger = log;

    if (lsp == nullptr)
        return;

    if (!isInitialized)
    {
        initializeLSP(path);
        isInitialized = true;
    }

    std::string uri = "file://" + path.toStdString();
    std::string code = m_editor->toPlainText().toStdString();
    std::string lang;

    if (language == "Java")
        lang = "java";
    else if (language == "Python")
        lang = "python";
    else
    {
        LOG_WARN_IF(language != "C++", "Unknown language " << language);
        lang = "cpp";
    }

    lsp->didOpen(uri, code, lang);
}

void LanguageServer::closeDocument()
{
    LOG_WARN_IF(!isDocumentOpen(), "Cannot close the document, No document was open");
    if (!isDocumentOpen())
        return;

    std::string uri = "file://" + openFile.toStdString();
    lsp->didClose(uri);

    openFile = "";
    logger = nullptr;
    m_editor = nullptr;
}

void LanguageServer::requestLinting()
{
    if (m_editor == nullptr || !isDocumentOpen())
        return;

    std::vector<TextDocumentContentChangeEvent> changes;
    TextDocumentContentChangeEvent e;
    e.text = m_editor->toPlainText().toStdString();
    changes.push_back(e);

    std::string uri = "file://" + openFile.toStdString();
    lsp->didChange(uri, changes, true);
}

void LanguageServer::requestCompletion(int lineNumber, int characterNumber, LSPCompleter *completer)
{
    if (m_editor == nullptr || !isDocumentOpen())
        return;

    CompletionContext context;
    Position pos;
    this->completer = completer;

    pos.line = lineNumber;
    pos.character = characterNumber;
    context.triggerKind = CompletionTriggerKind::Invoked;
    std::string s;
    context.triggerCharacter = s; // Take a reference, need to pass a empty character
    std::string uri = "file://" + openFile.toStdString();

    lsp->completion(uri, pos, context);
}

bool LanguageServer::isDocumentOpen() const
{
    return !openFile.isEmpty() && lsp != nullptr;
}

void LanguageServer::updateSettings()
{
    if (lsp != nullptr)
    {
        LOG_INFO("Killing LSP");
        lsp->shutdown();
        lsp->exit();
        delete lsp;
        lsp = nullptr;
    }

    if (m_editor != nullptr)
        m_editor->clearSquiggle();

    if (shouldCreateClient())
    {
        createClient();

        performConnection();
        initializeLSP(openFile);

        LOG_INFO("Recreated Language server Process");
        if (m_editor != nullptr)
        {
            auto *tmpEditor = m_editor;
            auto tmpPath = openFile;
            auto *tmpLog = logger;
            if (isDocumentOpen())
                closeDocument();
            openDocument(tmpPath, tmpEditor, tmpLog);
            LOG_INFO("Reopened document after restart");
        }
    }
}

void LanguageServer::updatePath(QString const &newPath)
{
    if (lsp == nullptr || (openFile == newPath))
        return;
    auto *tmpLogger = logger;
    auto *tmpEditor = m_editor;
    closeDocument();
    openDocument(newPath, tmpEditor, tmpLogger);
}

// Private methods
bool LanguageServer::shouldCreateClient()
{
    return SettingsManager::get("LSP/Use Linting " + language).toBool() ||
           SettingsManager::get("LSP/Use Autocomplete " + language).toBool();
}

void LanguageServer::createClient()
{
    delete lsp;
    auto program = SettingsManager::get("LSP/Path " + language).toString();
    auto args = QProcess::splitCommand(SettingsManager::get("LSP/Args " + language).toString().trimmed());
    lsp = new LSPClient(program, args);
}

void LanguageServer::performConnection()
{
    if (lsp == nullptr)
    {
        LOG_WARN("Skipping establishement of connections as lsp client is nullptr");
        return;
    }
    connect(lsp, &LSPClient::onError, this, &LanguageServer::onLSPServerErrorArrived);
    connect(lsp, &LSPClient::onRequest, this, &LanguageServer::onLSPServerRequestArrived);
    connect(lsp, &LSPClient::onServerError, this, &LanguageServer::onLSPServerProcessError);
    connect(lsp, &LSPClient::onResponse, this, &LanguageServer::onLSPServerResponseArrived);
    connect(lsp, &LSPClient::onNotify, this, &LanguageServer::onLSPServerNotificationArrived);
    connect(lsp, &LSPClient::onServerFinished, this, &LanguageServer::onLSPServerProcessFinished);
    connect(lsp, &LSPClient::newStderr, this, &LanguageServer::onLSPServerNewStderr);

    LOG_INFO("All language server connections have been established");
}

QCodeEditor::SeverityLevel LanguageServer::lspSeverity(int in)
{
    switch (in)
    {
    case 1:
        return QCodeEditor ::SeverityLevel::Error;
    case 2:
        return QCodeEditor::SeverityLevel::Warning;
    case 3:
        return QCodeEditor::SeverityLevel::Information;
    case 4:
        return QCodeEditor::SeverityLevel::Hint;
    }
    // Nothing matched
    return QCodeEditor::SeverityLevel::Error;
}

void LanguageServer::initializeLSP(QString const &filePath)
{
    QFileInfo info(filePath);
    std::string uri = "file://" + info.absoluteDir().absolutePath().toStdString();
    option<DocumentUri> rootUri(uri);
    lsp->initialize(rootUri);
}
// ---------------------------- LSP SLOTS ------------------------

void LanguageServer::onLSPServerNotificationArrived(QString const &method, QJsonObject const &param)
{
    if (method == "textDocument/publishDiagnostics" && m_editor != nullptr) // Linting
    {
        m_editor->clearSquiggle();
        QJsonArray doc = QJsonDocument::fromVariant(param.toVariantMap()).object()["diagnostics"].toArray();
        for (auto e : doc)
        {
            QString tooltip = e.toObject()["message"].toString();
            QCodeEditor::SeverityLevel level = lspSeverity(e.toObject()["severity"].toInt());

            auto beg = e.toObject()["range"].toObject()["start"].toObject();
            auto end = e.toObject()["range"].toObject()["end"].toObject();

            QPair<int, int> start;
            QPair<int, int> stop;

            start.first = beg["line"].toInt() + 1;
            start.second = beg["character"].toInt();

            stop.first = end["line"].toInt() + 1;
            stop.second = end["character"].toInt();

            m_editor->squiggle(level, start, stop,
                               tooltip.remove(" (fix available)")); // We do not provide quick fix so remove this text.
        }
    }
}

void LanguageServer::onLSPServerResponseArrived(QJsonObject const &method, // NOLINT: It can be made static.
                                                QJsonObject const &param)
{
    auto parameters = QJsonDocument::fromVariant(param.toVariantMap());
    if (parameters.isObject() && param.keys().contains("capabilities"))
    {
        QJsonObject capabilities = parameters["capabilities"].toObject();
        if (capabilities.keys().contains("completionProvider"))
        {
            QJsonObject completionProvider = capabilities["completionProvider"].toObject();
            if (completionProvider.keys().contains("triggerCharacters"))
            {
                auto chars = completionProvider["triggerCharacters"].toArray();
                for (auto character : chars)
                    completer->addTriggerCharacter(character.toString())

            } // else this language does not have trigger characters
        }
        else
        {
            LOG_WARN("LSP Program does not supports completion");
        }
    }
    qDebug().noquote().nospace() << parameters.toJson();
    if (parameters.isObject() && param.keys().contains("items") &&
        param.keys().contains("isIncomplete")) // Completion list
    {
        completer->clearCompletion();
        QStringList list;
        QJsonArray arr = parameters["items"].toArray();
        for (auto e : arr)
        {
            auto obj = e.toObject();
            auto kind = obj["kind"].toInt();
            auto label = obj["label"].toString();

            qDebug() << "Kind: " << kind << "->" << label;
            list.append(label.trimmed());
        }

        qDebug() << "Completion end";
        completer->addCompletion(list);
    }
}

void LanguageServer::onLSPServerRequestArrived(QString const &method, // NOLINT: It can be made static.
                                               QJsonObject const &param, QJsonObject const &id)
{
    LOG_INFO("Request from Sever has arrived. " << INFO_OF(method));
}

void LanguageServer::onLSPServerErrorArrived(QJsonObject const &id, QJsonObject const &error)
{
    QString ID;
    QString ERR;
    ID = QJsonDocument::fromVariant(id.toVariantMap()).toJson();
    ERR = QJsonDocument::fromVariant(error.toVariantMap()).toJson();

    LOG_ERR("ID is \n" << ID);
    LOG_ERR("ERR is \n" << ERR);

    qDebug() << id;
    qDebug() << error;

    if (logger != nullptr)
        logger->error(tr("Language Server [%1]").arg(language),
                      tr("Language server sent an error. Please check log for details."));
}

void LanguageServer::onLSPServerProcessError(QProcess::ProcessError const &error)
{
    LOG_WARN_IF(error == QProcess::Crashed, "LSP Process errored out " << INFO_OF(error));
    LOG_ERR_IF(error != QProcess::Crashed, "LSP Process errored out " << INFO_OF(error));
    if (logger == nullptr)
        return;
    switch (error)
    {
    case QProcess::FailedToStart:
        logger->error(tr("Language Server [%1]").arg(language),
                      tr("Failed to start LSP Process. Have you set the path to the Language Server program at %1?")
                          .arg(SettingsManager::getPathText("LSP/Path " + language)),
                      false);
        break;
    case QProcess::Crashed:
        break;
    case QProcess::Timedout:
        logger->error(tr("Language Server [%1]").arg(language), tr("LSP Process timed out"));
        break;
    case QProcess::ReadError:
        logger->error(tr("Language Server [%1]").arg(language), tr("LSP Process Read Error"));
        break;
    case QProcess::WriteError:
        logger->error(tr("Language Server [%1]").arg(language), tr("LSP Process Write Error"));
        break;
    case QProcess::UnknownError:
        logger->error(tr("Language Server [%1]").arg(language), tr("An unknown error has occurred in LSP Process"));
        break;
    }
}

void LanguageServer::onLSPServerProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    LOG_INFO_IF(exitCode == 0, "LSP Finished with exit code " << exitCode << INFO_OF(language) << INFO_OF(status));
    LOG_WARN_IF(exitCode != 0, "LSP Finished with exit code " << exitCode << INFO_OF(language) << INFO_OF(status));
}

void LanguageServer::onLSPServerNewStderr(const QString &content) // NOLINT: It can be made static
{
    LOG_INFO(content);
}
} // namespace Extensions
