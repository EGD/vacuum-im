#ifndef XMPPSTREAM_H
#define XMPPSTREAM_H

#include <QTimer>
#include <QDomDocument>
#include <definations/namespaces.h>
#include <interfaces/ixmppstreams.h>
#include <interfaces/iconnectionmanager.h>
#include <utils/errorhandler.h>
#include <utils/versionparser.h>
#include "streamparser.h"

enum StreamState {
  SS_OFFLINE, 
  SS_CONNECTING, 
  SS_INITIALIZE, 
  SS_FEATURES, 
  SS_ONLINE,
  SS_ERROR
}; 

class XmppStream : 
  public QObject,
  public IXmppStream
{
  Q_OBJECT;
  Q_INTERFACES(IXmppStream);
public:
  XmppStream(IXmppStreams *AXmppStreams, const Jid &AStreamJid);
  ~XmppStream();
  virtual QObject *instance() { return this; }
  //IXmppStream
  virtual bool isOpen() const;
  virtual bool open();
  virtual void close();
  virtual void abort(const QString &AError);
  virtual QString streamId() const;
  virtual QString errorString() const;;
  virtual Jid streamJid() const;
  virtual void setStreamJid(const Jid &AJid);
  virtual QString password() const;
  virtual void setPassword(const QString &APassword);
  virtual QString defaultLang() const;
  virtual void setDefaultLang(const QString &ADefLang);
  virtual IConnection *connection() const;
  virtual void setConnection(IConnection *AConnection);
  virtual void insertFeature(IStreamFeature *AFeature);
  virtual QList<IStreamFeature *> features() const;
  virtual void removeFeature(IStreamFeature *AFeature);
  virtual qint64 sendStanza(const Stanza &AStanza);
signals:
  void opened();
  void element(const QDomElement &elem);
  void consoleElement(const QDomElement &AElem, bool ADirectionOut);
  void aboutToClose();
  void closed();
  void error(const QString &AError);
  void jidAboutToBeChanged(const Jid &AAfter);
  void jidChanged(const Jid &ABefour);
  void connectionAdded(IConnection *AConnection);
  void connectionRemoved(IConnection *AConnection);
  void featureAdded(IStreamFeature *AFeature);
  void featureRemoved(IStreamFeature *AFeature);
  void streamDestroyed();
protected:
  void startStream();
  void processFeatures();
  IStreamFeature *getStreamFeature(const QString &AFeatureNS);
  void sortFeature(IStreamFeature *AFeature = NULL);
  bool startFeature(const QString &AFeatureNS, const QDomElement &AFeatureElem);
  bool hookFeatureData(QByteArray &AData, IStreamFeature::Direction ADirection);
  bool hookFeatureElement(QDomElement &AElem, IStreamFeature::Direction ADirection);
  qint64 sendData(const QByteArray &AData);
  QByteArray receiveData(qint64 ABytes);
  void showInConsole(const QDomElement &AElem, IStreamFeature::Direction ADirection);
protected slots:
  //IStreamConnection
  void onConnectionConnected();
  void onConnectionReadyRead(qint64 ABytes);
  void onConnectionError(const QString &AError);
  void onConnectionDisconnected();
  //StreamParser
  void onParserOpened(QDomElement AElem);
  void onParserElement(QDomElement AElem);
  void onParserError(const QString &AError);
  void onParserClosed();
  //IStreamFeature
  void onFeatureReady(bool ARestart);
  void onFeatureError(const QString &AError);
  //KeepAlive
  void onKeepAliveTimeout();
private:
  IXmppStreams *FXmppStreams;
  IConnection *FConnection;
private:
  QDomElement FFeaturesElement;
  IStreamFeature *FActiveFeature;
  QList<IStreamFeature *>	FFeatures; 
private:
  bool FOpen;
  Jid FStreamJid;
  Jid FOfflineJid;
  QString FStreamId; 
  QString FPassword;
  QString FSessionPassword;
  QString FDefLang;
  QString FErrorString;
  StreamParser FParser;
  QTimer FKeepAliveTimer;
  StreamState FStreamState;
};

#endif // XMPPSTREAM_H
