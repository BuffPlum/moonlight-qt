#pragma once

#include "backend/nvcomputer.h"
#include "protocol/file_mapping_client.h"

#include <atomic>
#include <memory>

class FileMappingClient;

class FileMappingProtocolAdapter : public FileMapping::ProtocolClient
{
public:
    explicit FileMappingProtocolAdapter(NvComputer computer,
                                        const std::atomic_bool* cancelRequested = nullptr);
    ~FileMappingProtocolAdapter() override;

    FileMapping::Capability fetchCapability(int timeoutMs) override;
    FileMapping::Error connectSession(const FileMapping::Capability& capability, int timeoutMs) override;
    QList<FileMapping::RemoteMapping> mappings() const override;
    FileMapping::ListResult list(const QString& mappingId, const QString& path, int timeoutMs) override;
    FileMapping::StatResult stat(const QString& mappingId, const QString& path, int timeoutMs) override;
    FileMapping::ReadResult read(const QString& mappingId,
                                 const QString& path,
                                 quint64 offset,
                                 quint32 length,
                                 int timeoutMs) override;
    FileMapping::PathResult mkdir(const QString& mappingId,
                                  const QString& path,
                                  FileMapping::ConflictPolicy conflictPolicy,
                                  int timeoutMs) override;
    FileMapping::PathResult rename(const QString& mappingId,
                                   const QString& path,
                                   const QString& destinationPath,
                                   int timeoutMs) override;
    FileMapping::Error remove(const QString& mappingId,
                              const QString& path,
                              bool recursive,
                              int timeoutMs) override;
    FileMapping::WriteResult write(const QString& mappingId,
                                   const QString& path,
                                   const QString& uploadId,
                                   quint64 offset,
                                   quint64 totalSize,
                                    const QByteArray& data,
                                    bool begin,
                                    bool complete,
                                    FileMapping::ConflictPolicy conflictPolicy,
                                    int timeoutMs) override;

private:
    FileMappingClient& client();

    NvComputer m_Computer;
    const std::atomic_bool* m_CancelRequested;
    std::unique_ptr<FileMappingClient> m_Client;
    QList<FileMapping::RemoteMapping> m_Mappings;
};
