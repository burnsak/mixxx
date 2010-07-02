/**
 * \file soundsourceflac.cpp
 * \author Bill Good <bkgood at gmail dot com>
 * \date May 22, 2010
 */

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include <cstring> // memcpy
#include <QtDebug>

#include "soundsourceflac.h"

SoundSourceFLAC::SoundSourceFLAC(QString filename)
    : SoundSource(filename)
    , m_file(filename)
    , m_decoder(NULL)
    , m_samples(0)
    , m_bps(0)
    , m_flacBuffer(NULL)
    , m_flacBufferLength(0)
    , m_leftoverBuffer(NULL)
    , m_leftoverBufferLength(0) {
}

SoundSourceFLAC::~SoundSourceFLAC() {
    if (m_flacBuffer != NULL) {
        delete [] m_flacBuffer;
        m_flacBuffer = NULL;
    }
    if (m_leftoverBuffer != NULL) {
        delete [] m_leftoverBuffer;
        m_leftoverBuffer = NULL;
    }
    if (m_decoder) {
        FLAC__stream_decoder_finish(m_decoder);
        FLAC__stream_decoder_delete(m_decoder); // frees memory
        m_decoder = NULL; // probably not necessary
    }
}

// soundsource overrides
int SoundSourceFLAC::open() {
    m_file.open(QIODevice::ReadOnly);
    m_decoder = FLAC__stream_decoder_new();
    if (m_decoder == NULL) {
        qDebug() << "SSFLAC: decoder allocation failed!";
        return ERR;
    }
    if (!FLAC__stream_decoder_set_metadata_respond(m_decoder,
                FLAC__METADATA_TYPE_VORBIS_COMMENT)) {
        qDebug() << "SSFLAC: set metadata responde to vorbis comments failed";
        return ERR;
    }
    FLAC__StreamDecoderInitStatus initStatus;
    initStatus = FLAC__stream_decoder_init_stream(
        m_decoder, FLAC_read_cb, FLAC_seek_cb, FLAC_tell_cb, FLAC_length_cb,
        FLAC_eof_cb, FLAC_write_cb, FLAC_metadata_cb, FLAC_error_cb,
        (void*) this);
    if (initStatus != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        qDebug() << "SSFLAC: decoder init failed!";
        FLAC__stream_decoder_finish(m_decoder);
        FLAC__stream_decoder_delete(m_decoder);
        m_decoder = NULL;
        return ERR;
    }
    if (!FLAC__stream_decoder_process_until_end_of_metadata(m_decoder)) {
        qDebug() << "SSFLAC: process to end of meta failed!";
        qDebug() << "SSFLAC: decoder state: " << FLAC__stream_decoder_get_state(m_decoder);
        FLAC__stream_decoder_finish(m_decoder);
        FLAC__stream_decoder_delete(m_decoder);
        m_decoder = NULL;
        return ERR;
    } // now number of samples etc. should be populated
    if (m_bps != 16) {
        qDebug() << "SoundSourceFLAC only supports FLAC files encoded at 16 bits per sample.";
        FLAC__stream_decoder_finish(m_decoder);
        FLAC__stream_decoder_delete(m_decoder);
        m_decoder = NULL;
        return ERR;
    }
    if (m_flacBuffer == NULL) {
        m_flacBuffer = new FLAC__int16[m_maxBlocksize * m_iChannels];
    }
    if (m_leftoverBuffer == NULL) {
        m_leftoverBuffer = new FLAC__int16[m_maxBlocksize * m_iChannels];
    }
    qDebug() << "SSFLAC: Total samples: " << m_samples;
    qDebug() << "SSFLAC: Sampling rate: " << m_iSampleRate << " Hz";
    qDebug() << "SSFLAC: Channels: " << m_iChannels;
    qDebug() << "SSFLAC: BPS: " << m_bps;
    return OK;
}

long SoundSourceFLAC::seek(long filepos) {
    if (!m_decoder) return 0;
    FLAC__bool seekResult;
    // important division here, filepos is in audio samples (i.e. shorts)
    // but libflac expects a number in time samples. I _think_ this should
    // be hard-coded at two because *2 is the assumption the caller makes
    // -- bkgood
    seekResult = FLAC__stream_decoder_seek_absolute(m_decoder, filepos / 2);
    m_leftoverBufferLength = 0; // clear internal buffer since we moved
    return filepos;
}

unsigned int SoundSourceFLAC::read(unsigned long size, const SAMPLE *destination) {
    if (!m_decoder) return 0;
    SAMPLE *destBuffer = const_cast<SAMPLE*>(destination);
    unsigned int samplesWritten = 0;
    unsigned int i = 0;
    while (samplesWritten < size) {
        // if our buffer from libflac is empty (either because we explicitly cleared
        // it or because we've simply used all the samples), ask for a new buffer
        if (m_flacBufferLength == 0) {
            i = 0;
            if (!FLAC__stream_decoder_process_single(m_decoder)) {
                qDebug() << "SSFLAC: decoder_process_single returned false";
                break;
            } else if (m_flacBufferLength == 0) {
                // EOF
                break;
            }
        }
        destBuffer[samplesWritten++] = m_flacBuffer[i++];
        --m_flacBufferLength;
    }
    if (m_flacBufferLength != 0) {
        memcpy(m_leftoverBuffer, &m_flacBuffer[i],
                m_flacBufferLength * sizeof(m_flacBuffer[0])); // safe because leftoverBuffer
                                                               // is as long as flacbuffer
        memcpy(m_flacBuffer, m_leftoverBuffer,
                m_flacBufferLength * sizeof(m_leftoverBuffer[0]));
        // memmove = malloc = expensive. Far more expensive than the 128kbits that
        // goes into each of these channel buffers
        // this whole if block could go away if this just used a ring buffer but I'd
        // rather do that after I've gotten off the inital happiness of getting this right,
        // if I see SIGSEGV one more time I'll pop -- bkgood
    }
    return samplesWritten;
}

inline unsigned long SoundSourceFLAC::length() {
    return m_samples * m_iChannels;
}

int SoundSourceFLAC::parseHeader() {
    open();
    setType("FLAC");
    setBitrate(m_iSampleRate * 16 * m_iChannels / 1000); // 16 = bps
    setDuration(m_samples / m_iSampleRate);
    foreach (QString i, m_tags) {
        setTag(i);
    }
    return OK;
}

void SoundSourceFLAC::setTag(const QString &tag) {
    QString key = tag.left(tag.indexOf("=")).toUpper();
    QString value = tag.right(tag.length() - tag.indexOf("=") - 1);
    // standard here: http://www.xiph.org/vorbis/doc/v-comment.html
    if (key == "ARTIST") {
        m_sArtist = value;
    } else if (key == "TITLE") {
        m_sTitle = value;
    } else if (key == "ALBUM") {
        m_sAlbum = value;
    } else if (key == "COMMENT") { // this doesn't exist in standard vorbis comments
        m_sComment = value;
    } else if (key == "DATE") {
        m_sYear = value;
    } else if (key == "GENRE") {
        m_sGenre = value;
    } else if (key == "TRACKNUMBER") {
        m_sTrackNumber = value;
    } else if (key == "BPM") { // this doesn't exist in standard vorbis comments
        m_fBPM = value.toFloat();
    }
}

// static
QList<QString> SoundSourceFLAC::supportedFileExtensions() {
    QList<QString> list;
    list.push_back("flac");
    return list;
}


// flac callback methods
FLAC__StreamDecoderReadStatus SoundSourceFLAC::flacRead(FLAC__byte buffer[], size_t *bytes) {
    *bytes = m_file.read((char*) buffer, *bytes);
    if (*bytes > 0) {
        return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    } else if (*bytes == 0) {
        return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    } else {
        return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    }
}

FLAC__StreamDecoderSeekStatus SoundSourceFLAC::flacSeek(FLAC__uint64 offset) {
    if (m_file.seek(offset)) {
        return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
    } else {
        return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
    }
}

FLAC__StreamDecoderTellStatus SoundSourceFLAC::flacTell(FLAC__uint64 *offset) {
    if (m_file.isSequential()) {
        return FLAC__STREAM_DECODER_TELL_STATUS_UNSUPPORTED;
    }
    *offset = m_file.pos();
    return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

FLAC__StreamDecoderLengthStatus SoundSourceFLAC::flacLength(FLAC__uint64 *length) {
    if (m_file.isSequential()) {
        return FLAC__STREAM_DECODER_LENGTH_STATUS_UNSUPPORTED;
    }
    *length = m_file.size();
    return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}

FLAC__bool SoundSourceFLAC::flacEOF() {
    if (m_file.isSequential()) {
        return false;
    }
    return m_file.atEnd();
}

FLAC__StreamDecoderWriteStatus SoundSourceFLAC::flacWrite(const FLAC__Frame *frame,
        const FLAC__int32 *const buffer[]) {
    unsigned int i;
    m_flacBufferLength = 0;
    if (frame->header.channels > 1) {
        // stereo (or greater)
        for (i = 0; i < frame->header.blocksize; ++i) {
            m_flacBuffer[m_flacBufferLength++] = buffer[0][i]; // left channel
            m_flacBuffer[m_flacBufferLength++] = buffer[1][i]; // right channel
        }
    } else {
        // mono
        for (i = 0; i < frame->header.blocksize; ++i) {
            m_flacBuffer[m_flacBufferLength++] = buffer[0][i]; // left channel
            m_flacBuffer[m_flacBufferLength++] = buffer[0][i]; // mono channel
        }
    }
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE; // can't anticipate any errors here
}

void SoundSourceFLAC::flacMetadata(const FLAC__StreamMetadata *metadata) {
    switch (metadata->type) {
    case FLAC__METADATA_TYPE_STREAMINFO:
        m_samples = metadata->data.stream_info.total_samples;
        m_iChannels = metadata->data.stream_info.channels;
        m_iSampleRate = metadata->data.stream_info.sample_rate;
        m_bps = metadata->data.stream_info.bits_per_sample;
        m_minBlocksize = metadata->data.stream_info.min_blocksize;
        m_maxBlocksize = metadata->data.stream_info.max_blocksize;
        m_minFramesize = metadata->data.stream_info.min_framesize;
        m_maxFramesize = metadata->data.stream_info.max_framesize;
        qDebug() << "FLAC file " << m_qFilename;
        qDebug() << m_iChannels << " @ " << m_iSampleRate << " Hz, " << m_samples
            << " total, " << m_bps << " bps";
        qDebug() << "Blocksize in [" << m_minBlocksize << ", " << m_maxBlocksize
            << "], Framesize in [" << m_minFramesize << ", " << m_maxFramesize << "]";
        break;
    case FLAC__METADATA_TYPE_VORBIS_COMMENT:
        for (unsigned int i = 0; i < metadata->data.vorbis_comment.num_comments; ++i) {
            m_tags.append(QString::fromUtf8(
                        (const char*) metadata->data.vorbis_comment.comments[i].entry,
                        metadata->data.vorbis_comment.comments[i].length));
        }
        break;
    default:
        // don't care, and libflac won't send us any others anyway...
        break;
    }
}

void SoundSourceFLAC::flacError(FLAC__StreamDecoderErrorStatus status) {
    qDebug() << "SSFLAC::flacError";
    QString error;
    // not much can be done at this point -- luckly the decoder seems to be
    // pretty forgiving -- bkgood
    switch (status) {
    case FLAC__STREAM_DECODER_ERROR_STATUS_LOST_SYNC:
        error = "STREAM_DECODER_ERROR_STATUS_LOST_SYNC";
        break;
    case FLAC__STREAM_DECODER_ERROR_STATUS_BAD_HEADER:
        error = "STREAM_DECODER_ERROR_STATUS_BAD_HEADER";
        break;
    case FLAC__STREAM_DECODER_ERROR_STATUS_FRAME_CRC_MISMATCH:
        error = "STREAM_DECODER_ERROR_STATUS_FRAME_CRC_MISMATCH";
        break;
    case FLAC__STREAM_DECODER_ERROR_STATUS_UNPARSEABLE_STREAM:
        error = "STREAM_DECODER_ERROR_STATUS_UNPARSEABLE_STREAM";
        break;
    }
    qDebug() << "SSFLAC got error" << error << "from libFLAC for file"
        << m_file.fileName();
    // not much else to do here... whatever function that initiated whatever
    // decoder method resulted in this error will return an error, and the caller
    // will bail. libFLAC docs say to not close the decoder here -- bkgood
}

// begin callbacks (have to be regular functions because normal libFLAC isn't C++-aware)

FLAC__StreamDecoderReadStatus FLAC_read_cb(const FLAC__StreamDecoder*, FLAC__byte buffer[], size_t *bytes, void *client_data) {
    return ((SoundSourceFLAC*) client_data)->flacRead(buffer, bytes);
}

FLAC__StreamDecoderSeekStatus FLAC_seek_cb(const FLAC__StreamDecoder*, FLAC__uint64 absolute_byte_offset, void *client_data) {
    return ((SoundSourceFLAC*) client_data)->flacSeek(absolute_byte_offset);
}

FLAC__StreamDecoderTellStatus FLAC_tell_cb(const FLAC__StreamDecoder*, FLAC__uint64 *absolute_byte_offset, void *client_data) {
    return ((SoundSourceFLAC*) client_data)->flacTell(absolute_byte_offset);
}

FLAC__StreamDecoderLengthStatus FLAC_length_cb(const FLAC__StreamDecoder*, FLAC__uint64 *stream_length, void *client_data) {
    return ((SoundSourceFLAC*) client_data)->flacLength(stream_length);
}

FLAC__bool FLAC_eof_cb(const FLAC__StreamDecoder*, void *client_data) {
    return ((SoundSourceFLAC*) client_data)->flacEOF();
}

FLAC__StreamDecoderWriteStatus FLAC_write_cb(const FLAC__StreamDecoder*, const FLAC__Frame *frame, const FLAC__int32 *const buffer[], void *client_data) {
    return ((SoundSourceFLAC*) client_data)->flacWrite(frame, buffer);
}

void FLAC_metadata_cb(const FLAC__StreamDecoder*, const FLAC__StreamMetadata *metadata, void *client_data) {
    ((SoundSourceFLAC*) client_data)->flacMetadata(metadata);
}

void FLAC_error_cb(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus status, void *client_data) {
    ((SoundSourceFLAC*) client_data)->flacError(status);
}
// end callbacks
