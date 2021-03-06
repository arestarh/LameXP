///////////////////////////////////////////////////////////////////////////////
// LameXP - Audio Encoder Front-End
// Copyright (C) 2004-2011 LoRd_MuldeR <MuldeR2@GMX.de>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//
// http://www.gnu.org/licenses/gpl-2.0.txt
///////////////////////////////////////////////////////////////////////////////

#include "Encoder_AAC.h"

#include "Global.h"
#include "Model_Settings.h"

#include <QProcess>
#include <QDir>

AACEncoder::AACEncoder(void)
:
	m_binary_enc(lamexp_lookup_tool("neroAacEnc.exe")),
	m_binary_tag(lamexp_lookup_tool("neroAacTag.exe")),
	m_binary_sox(lamexp_lookup_tool("sox.exe"))
{
	if(m_binary_enc.isEmpty() || m_binary_tag.isEmpty() || m_binary_sox.isEmpty())
	{
		throw "Error initializing AAC encoder. Tool 'neroAacEnc.exe' is not registred!";
	}

	m_configProfile = 0;
	m_configEnable2Pass = true;
}

AACEncoder::~AACEncoder(void)
{
}

bool AACEncoder::encode(const QString &sourceFile, const AudioFileModel &metaInfo, const QString &outputFile, volatile bool *abortFlag)
{
	const unsigned int fileDuration = (metaInfo.fileDuration() > 0) ? metaInfo.fileDuration() : detectLength(sourceFile, abortFlag);
	
	QProcess process;
	QStringList args;
	const QString baseName = QFileInfo(outputFile).fileName();

	switch(m_configRCMode)
	{
	case SettingsModel::VBRMode:
		args << "-q" << QString().sprintf("%.2f", qBound(0.0, static_cast<double>(m_configBitrate * 5) / 100.0, 1.0));
		break;
	case SettingsModel::ABRMode:
		args << "-br" << QString::number(qMax(32, qMin(500, (m_configBitrate * 8))) * 1000);
		break;
	case SettingsModel::CBRMode:
		args << "-cbr" << QString::number(qMax(32, qMin(500, (m_configBitrate * 8))) * 1000);
		break;
	default:
		throw "Bad rate-control mode!";
		break;
	}

	if(m_configEnable2Pass && (m_configRCMode == SettingsModel::ABRMode))
	{
		args << "-2pass";
	}
	
	switch(m_configProfile)
	{
	case 1:
		args << "-lc"; //Forces use of LC AAC profile
		break;
	case 2:
		args << "-he"; //Forces use of HE AAC profile
		break;
	case 3:
		args << "-hev2"; //Forces use of HEv2 AAC profile
		break;
	}

	if(!m_configCustomParams.isEmpty()) args << m_configCustomParams.split(" ", QString::SkipEmptyParts);

	args << "-if" << QDir::toNativeSeparators(sourceFile);
	args << "-of" << QDir::toNativeSeparators(outputFile);

	if(!startProcess(process, m_binary_enc, args))
	{
		return false;
	}

	bool bTimeout = false;
	bool bAborted = false;
	int prevProgress = -1;


	QRegExp regExp("Processed\\s+(\\d+)\\s+seconds");
	QRegExp regExp_pass1("First\\s+pass:\\s+processed\\s+(\\d+)\\s+seconds");
	QRegExp regExp_pass2("Second\\s+pass:\\s+processed\\s+(\\d+)\\s+seconds");

	while(process.state() != QProcess::NotRunning)
	{
		if(*abortFlag)
		{
			process.kill();
			bAborted = true;
			emit messageLogged("\nABORTED BY USER !!!");
			break;
		}
		process.waitForReadyRead(m_processTimeoutInterval);
		if(!process.bytesAvailable() && process.state() == QProcess::Running)
		{
			process.kill();
			qWarning("NeroAacEnc process timed out <-- killing!");
			emit messageLogged("\nPROCESS TIMEOUT !!!");
			bTimeout = true;
			break;
		}
		while(process.bytesAvailable() > 0)
		{
			QByteArray line = process.readLine();
			QString text = QString::fromUtf8(line.constData()).simplified();
			if(regExp_pass1.lastIndexIn(text) >= 0)
			{
				bool ok = false;
				int progress = regExp_pass1.cap(1).toInt(&ok);
				if(ok && (fileDuration > 0))
				{
					int newProgress = qRound((static_cast<double>(progress) / static_cast<double>(fileDuration)) * 50.0);
					if(newProgress > prevProgress)
					{
						emit statusUpdated(newProgress);
						prevProgress = qMin(newProgress + 2, 99);
					}
				}
			}
			else if(regExp_pass2.lastIndexIn(text) >= 0)
			{
				bool ok = false;
				int progress = regExp_pass2.cap(1).toInt(&ok);
				if(ok && (fileDuration > 0))
				{
					int newProgress = qRound((static_cast<double>(progress) / static_cast<double>(fileDuration)) * 50.0) + 50;
					if(newProgress > prevProgress)
					{
						emit statusUpdated(newProgress);
						prevProgress = qMin(newProgress + 2, 99);
					}
				}
			}
			else if(regExp.lastIndexIn(text) >= 0)
			{
				bool ok = false;
				int progress = regExp.cap(1).toInt(&ok);
				if(ok && (fileDuration > 0))
				{
					int newProgress = qRound((static_cast<double>(progress) / static_cast<double>(fileDuration)) * 100.0);
					if(newProgress > prevProgress)
					{
						emit statusUpdated(newProgress);
						prevProgress = qMin(newProgress + 2, 99);
					}
				}
			}
			else if(!text.isEmpty())
			{
				emit messageLogged(text);
			}
		}
	}

	process.waitForFinished();
	if(process.state() != QProcess::NotRunning)
	{
		process.kill();
		process.waitForFinished(-1);
	}
	
	emit statusUpdated(100);
	emit messageLogged(QString().sprintf("\nExited with code: 0x%04X", process.exitCode()));

	if(bTimeout || bAborted || process.exitStatus() != QProcess::NormalExit)
	{
		return false;
	}

	emit messageLogged("\n-------------------------------\n");
	
	args.clear();
	args << QDir::toNativeSeparators(outputFile);

	if(!metaInfo.fileName().isEmpty()) args << QString("-meta:title=%1").arg(metaInfo.fileName());
	if(!metaInfo.fileArtist().isEmpty()) args << QString("-meta:artist=%1").arg(metaInfo.fileArtist());
	if(!metaInfo.fileAlbum().isEmpty()) args << QString("-meta:album=%1").arg(metaInfo.fileAlbum());
	if(!metaInfo.fileGenre().isEmpty()) args << QString("-meta:genre=%1").arg(metaInfo.fileGenre());
	if(!metaInfo.fileComment().isEmpty()) args << QString("-meta:comment=%1").arg(metaInfo.fileComment());
	if(metaInfo.fileYear()) args << QString("-meta:year=%1").arg(QString::number(metaInfo.fileYear()));
	if(metaInfo.filePosition()) args << QString("-meta:track=%1").arg(QString::number(metaInfo.filePosition()));
	if(!metaInfo.fileCover().isEmpty()) args << QString("-add-cover:%1:%2").arg("front", metaInfo.fileCover());
	
	if(!startProcess(process, m_binary_tag, args))
	{
		return false;
	}

	bTimeout = false;

	while(process.state() != QProcess::NotRunning)
	{
		if(*abortFlag)
		{
			process.kill();
			bAborted = true;
			emit messageLogged("\nABORTED BY USER !!!");
			break;
		}
		process.waitForReadyRead(m_processTimeoutInterval);
		if(!process.bytesAvailable() && process.state() == QProcess::Running)
		{
			process.kill();
			qWarning("NeroAacTag process timed out <-- killing!");
			emit messageLogged("\nPROCESS TIMEOUT !!!");
			bTimeout = true;
			break;
		}
		while(process.bytesAvailable() > 0)
		{
			QByteArray line = process.readLine();
			QString text = QString::fromUtf8(line.constData()).simplified();
			if(!text.isEmpty())
			{
				emit messageLogged(text);
			}
		}
	}

	process.waitForFinished();
	if(process.state() != QProcess::NotRunning)
	{
		process.kill();
		process.waitForFinished(-1);
	}
		
	emit messageLogged(QString().sprintf("\nExited with code: 0x%04X", process.exitCode()));

	if(bTimeout || bAborted || process.exitStatus() != QProcess::NormalExit)
	{
		return false;
	}

	return true;
}

unsigned int AACEncoder::detectLength(const QString &sourceFile, volatile bool *abortFlag)
{
	unsigned int duration = 0;
	
	QProcess process;
	QStringList args;

	args << "--i" << sourceFile;

	if(!startProcess(process, m_binary_sox, args))
	{
		return duration;
	}

	bool bTimeout = false;
	bool bAborted = false;

	QRegExp regExp("Duration\\s*:\\s*(\\d\\d):(\\d\\d):(\\d\\d)\\.(\\d\\d)", Qt::CaseInsensitive);

	while(process.state() != QProcess::NotRunning)
	{
		if(*abortFlag)
		{
			process.kill();
			bAborted = true;
			emit messageLogged("\nABORTED BY USER !!!");
			break;
		}
		process.waitForReadyRead(m_processTimeoutInterval);
		if(!process.bytesAvailable() && process.state() == QProcess::Running)
		{
			process.kill();
			qWarning("SoX process timed out <-- killing!");
			emit messageLogged("\nPROCESS TIMEOUT !!!");
			bTimeout = true;
			break;
		}
		while(process.bytesAvailable() > 0)
		{
			QByteArray line = process.readLine();
			QString text = QString::fromUtf8(line.constData()).simplified();
			if(regExp.lastIndexIn(text) >= 0)
			{
				bool ok[4] = {false, false, false, false};
				unsigned int tmp1 = regExp.cap(1).toUInt(&ok[0]);
				unsigned int tmp2 = regExp.cap(2).toUInt(&ok[1]);
				unsigned int tmp3 = regExp.cap(3).toUInt(&ok[2]);
				unsigned int tmp4 = regExp.cap(4).toUInt(&ok[3]);
				if(ok[0] && ok[1] && ok[2] && ok[3])
				{
					duration = (tmp1 * 3600) + (tmp2 * 60) + tmp3 + qRound(static_cast<double>(tmp4) / 100.0);
				}
			}
			if(!text.isEmpty())
			{
				emit messageLogged(text);
			}
		}
	}

	process.waitForFinished();
	if(process.state() != QProcess::NotRunning)
	{
		process.kill();
		process.waitForFinished(-1);
	}

	//qWarning("Duration detected is: %u", duration);
	return duration;
}

QString AACEncoder::extension(void)
{
	return "mp4";
}

bool AACEncoder::isFormatSupported(const QString &containerType, const QString &containerProfile, const QString &formatType, const QString &formatProfile, const QString &formatVersion)
{
	if(containerType.compare("Wave", Qt::CaseInsensitive) == 0)
	{
		if(formatType.compare("PCM", Qt::CaseInsensitive) == 0)
		{
			return true;
		}
	}

	return false;
}

void AACEncoder::setProfile(int profile)
{
	m_configProfile = profile;
}

void AACEncoder::setEnable2Pass(bool enabled)
{
	m_configEnable2Pass = enabled;
}
