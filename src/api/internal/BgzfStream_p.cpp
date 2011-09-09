// ***************************************************************************
// BgzfStream_p.cpp (c) 2011 Derek Barnett
// Marth Lab, Department of Biology, Boston College
// All rights reserved.
// ---------------------------------------------------------------------------
// Last modified: 9 September 2011(DB)
// ---------------------------------------------------------------------------
// Based on BGZF routines developed at the Broad Institute.
// Provides the basic functionality for reading & writing BGZF files
// Replaces the old BGZF.* files to avoid clashing with other toolkits
// ***************************************************************************

#include <api/internal/BamDeviceFactory_p.h>
#include <api/internal/BgzfStream_p.h>
using namespace BamTools;
using namespace BamTools::Internal;

#include <cstring>
#include <algorithm>
#include <iostream>
using namespace std;

// constructor
BgzfStream::BgzfStream(void)
    : m_uncompressedBlockSize(Constants::BGZF_DEFAULT_BLOCK_SIZE)
    , m_compressedBlockSize(Constants::BGZF_MAX_BLOCK_SIZE)
    , m_blockLength(0)
    , m_blockOffset(0)
    , m_blockAddress(0)
    , m_uncompressedBlock(NULL)
    , m_compressedBlock(NULL)
    , m_isOpen(false)
    , m_isWriteOnly(false)
    , m_isWriteCompressed(true)
    , m_device(0)
    , m_stream(NULL)
{
    try {
        m_compressedBlock   = new char[m_compressedBlockSize];
        m_uncompressedBlock = new char[m_uncompressedBlockSize];
    } catch( std::bad_alloc& ba ) {
        fprintf(stderr, "BgzfStream ERROR: unable to allocate memory\n");
        exit(1);
    }
}

// destructor
BgzfStream::~BgzfStream(void) {
    if( m_compressedBlock   ) delete[] m_compressedBlock;
    if( m_uncompressedBlock ) delete[] m_uncompressedBlock;
}

// closes BGZF file
void BgzfStream::Close(void) {

    // skip if no device open
    if ( m_device == 0 )
        return;

    // if writing to file, flush the current BGZF block,
    // then write an empty block (as EOF marker)
    if ( m_device->IsOpen() && (m_device->Mode() == IBamIODevice::WriteOnly) ) {
        FlushBlock();
        int blockLength = DeflateBlock();
        m_device->Write(m_compressedBlock, blockLength);
    }

    // close device
    m_device->Close();

    // clean up & reset flags
    delete m_device;
    m_device = 0;
    m_isWriteCompressed = true;
    m_isOpen = false;
}

// compresses the current block
unsigned int BgzfStream::DeflateBlock(void) {

    // initialize the gzip header
    char* buffer = m_compressedBlock;
    memset(buffer, 0, 18);
    buffer[0]  = Constants::GZIP_ID1;
    buffer[1]  = (char)Constants::GZIP_ID2;
    buffer[2]  = Constants::CM_DEFLATE;
    buffer[3]  = Constants::FLG_FEXTRA;
    buffer[9]  = (char)Constants::OS_UNKNOWN;
    buffer[10] = Constants::BGZF_XLEN;
    buffer[12] = Constants::BGZF_ID1;
    buffer[13] = Constants::BGZF_ID2;
    buffer[14] = Constants::BGZF_LEN;

    // set compression level
    const int compressionLevel = ( m_isWriteCompressed ? Z_DEFAULT_COMPRESSION : 0 );

    // loop to retry for blocks that do not compress enough
    int inputLength = m_blockOffset;
    unsigned int compressedLength = 0;
    unsigned int bufferSize = m_compressedBlockSize;

    while ( true ) {

        // initialize zstream values
        z_stream zs;
        zs.zalloc    = NULL;
        zs.zfree     = NULL;
        zs.next_in   = (Bytef*)m_uncompressedBlock;
        zs.avail_in  = inputLength;
        zs.next_out  = (Bytef*)&buffer[Constants::BGZF_BLOCK_HEADER_LENGTH];
        zs.avail_out = bufferSize - Constants::BGZF_BLOCK_HEADER_LENGTH - Constants::BGZF_BLOCK_FOOTER_LENGTH;

        // initialize the zlib compression algorithm
        if ( deflateInit2(&zs,
                          compressionLevel,
                          Z_DEFLATED,
                          Constants::GZIP_WINDOW_BITS,
                          Constants::Z_DEFAULT_MEM_LEVEL,
                          Z_DEFAULT_STRATEGY) != Z_OK )
        {
            fprintf(stderr, "BgzfStream ERROR: zlib deflate initialization failed\n");
            exit(1);
        }

        // compress the data
        int status = deflate(&zs, Z_FINISH);
        if ( status != Z_STREAM_END ) {

            deflateEnd(&zs);

            // reduce the input length and try again
            if ( status == Z_OK ) {
                inputLength -= 1024;
                if ( inputLength < 0 ) {
                    fprintf(stderr, "BgzfStream ERROR: input reduction failed\n");
                    exit(1);
                }
                continue;
            }

            fprintf(stderr, "BgzfStream ERROR: zlib::deflateEnd() failed\n");
            exit(1);
        }

        // finalize the compression routine
        if ( deflateEnd(&zs) != Z_OK ) {
            fprintf(stderr, "BgzfStream ERROR: zlib::deflateEnd() failed\n");
            exit(1);
        }

        compressedLength = zs.total_out;
        compressedLength += Constants::BGZF_BLOCK_HEADER_LENGTH + Constants::BGZF_BLOCK_FOOTER_LENGTH;
        if ( compressedLength > Constants::BGZF_MAX_BLOCK_SIZE ) {
            fprintf(stderr, "BgzfStream ERROR: deflate overflow\n");
            exit(1);
        }

        break;
    }

    // store the compressed length
    BamTools::PackUnsignedShort(&buffer[16], (unsigned short)(compressedLength - 1));

    // store the CRC32 checksum
    unsigned int crc = crc32(0, NULL, 0);
    crc = crc32(crc, (Bytef*)m_uncompressedBlock, inputLength);
    BamTools::PackUnsignedInt(&buffer[compressedLength - 8], crc);
    BamTools::PackUnsignedInt(&buffer[compressedLength - 4], inputLength);

    // ensure that we have less than a block of data left
    int remaining = m_blockOffset - inputLength;
    if ( remaining > 0 ) {
        if ( remaining > inputLength ) {
            fprintf(stderr, "BgzfStream ERROR: after deflate, remainder too large\n");
            exit(1);
        }
        memcpy(m_uncompressedBlock, m_uncompressedBlock + inputLength, remaining);
    }

    // update block data
    m_blockOffset = remaining;

    // return result
    return compressedLength;
}

// flushes the data in the BGZF block
void BgzfStream::FlushBlock(void) {

    BT_ASSERT_X( m_device, "BgzfStream::FlushBlock() - attempting to flush to null device" );

    // flush all of the remaining blocks
    while ( m_blockOffset > 0 ) {

        // compress the data block
        unsigned int blockLength = DeflateBlock();

        // flush the data to our output stream
        unsigned int numBytesWritten = m_device->Write(m_compressedBlock, blockLength);
        if ( numBytesWritten != blockLength ) {
            fprintf(stderr, "BgzfStream ERROR: expected to write %u bytes during flushing, but wrote %u bytes\n",
                    blockLength, numBytesWritten);
            exit(1);
        }

        // update block data
        m_blockAddress += blockLength;
    }
}

// decompresses the current block
int BgzfStream::InflateBlock(const int& blockLength) {

    // inflate the data from compressed buffer into uncompressed buffer
    z_stream zs;
    zs.zalloc    = NULL;
    zs.zfree     = NULL;
    zs.next_in   = (Bytef*)m_compressedBlock + 18;
    zs.avail_in  = blockLength - 16;
    zs.next_out  = (Bytef*)m_uncompressedBlock;
    zs.avail_out = m_uncompressedBlockSize;

    int status = inflateInit2(&zs, Constants::GZIP_WINDOW_BITS);
    if ( status != Z_OK ) {
        fprintf(stderr, "BgzfStream ERROR: could not decompress block - zlib::inflateInit() failed\n");
        return -1;
    }

    status = inflate(&zs, Z_FINISH);
    if ( status != Z_STREAM_END ) {
        inflateEnd(&zs);
        fprintf(stderr, "BgzfStream ERROR: could not decompress block - zlib::inflate() failed\n");
        return -1;
    }

    status = inflateEnd(&zs);
    if ( status != Z_OK ) {
        fprintf(stderr, "BgzfStream ERROR: could not decompress block - zlib::inflateEnd() failed\n");
        return -1;
    }

    // return result
    return zs.total_out;
}

bool BgzfStream::IsOpen(void) const {
    if ( m_device == 0 )
        return false;
    return m_device->IsOpen();
}

bool BgzfStream::Open(const string& filename, const IBamIODevice::OpenMode mode) {

    // close current device if necessary
    Close();

    // sanity check
    BT_ASSERT_X( (m_device == 0), "BgzfStream::Open() - unable to properly close previous IO device" );

    // retrieve new IO device depending on filename
    m_device = BamDeviceFactory::CreateDevice(filename);

    // sanity check
    BT_ASSERT_X( m_device, "BgzfStream::Open() - unable to create IO device from filename" );

    // if device fails to open
    if ( !m_device->Open(mode) ) {
        cerr << "BgzfStream::Open() - unable to open IO device:" << endl;
        cerr << m_device->ErrorString();
        return false;
    }

    // otherwise, set flag & return true
    m_isOpen = true;
    m_isWriteOnly = ( mode == IBamIODevice::WriteOnly );
    return true;

}

// opens the BGZF file for reading (mode is either "rb" for reading, or "wb" for writing)
bool BgzfStream::Open(const string& filename, const char* mode) {

    // close current stream, if necessary, before opening next
    if ( m_isOpen ) Close();

    // determine open mode
    if ( strcmp(mode, "rb") == 0 )
        m_isWriteOnly = false;
    else if ( strcmp(mode, "wb") == 0)
        m_isWriteOnly = true;
    else {
        fprintf(stderr, "BgzfStream ERROR: unknown file mode: %s\n", mode);
        return false;
    }

    // open BGZF stream on a file
    if ( (filename != "stdin") && (filename != "stdout") && (filename != "-"))
        m_stream = fopen(filename.c_str(), mode);

    // open BGZF stream on stdin
    else if ( (filename == "stdin" || filename == "-") && (strcmp(mode, "rb") == 0 ) )
        m_stream = freopen(NULL, mode, stdin);

    // open BGZF stream on stdout
    else if ( (filename == "stdout" || filename == "-") && (strcmp(mode, "wb") == 0) )
        m_stream = freopen(NULL, mode, stdout);

    if ( !m_stream ) {
        fprintf(stderr, "BgzfStream ERROR: unable to open file %s\n", filename.c_str() );
        return false;
    }

    // set flag & return success
    m_isOpen = true;
    return true;
}

// reads BGZF data into a byte buffer
unsigned int BgzfStream::Read(char* data, const unsigned int dataLength) {

    if ( dataLength == 0 )
        return 0;

    // if stream not open for reading
    BT_ASSERT_X( m_device, "BgzfStream::Read() - trying to read from null device");
    if ( !m_device->IsOpen() || (m_device->Mode() != IBamIODevice::ReadOnly) )
        return 0;

    // read blocks as needed until desired data length is retrieved
    char* output = data;
    unsigned int numBytesRead = 0;
    while ( numBytesRead < dataLength ) {

        // determine bytes available in current block
        int bytesAvailable = m_blockLength - m_blockOffset;

        // read (and decompress) next block if needed
        if ( bytesAvailable <= 0 ) {
            if ( !ReadBlock() ) return -1;
            bytesAvailable = m_blockLength - m_blockOffset;
            if ( bytesAvailable <= 0 ) break;
        }

        // copy data from uncompressed source buffer into data destination buffer
        char* buffer   = m_uncompressedBlock;
        int copyLength = min( (int)(dataLength-numBytesRead), bytesAvailable );
        memcpy(output, buffer + m_blockOffset, copyLength);

        // update counters
        m_blockOffset += copyLength;
        output        += copyLength;
        numBytesRead  += copyLength;
    }

    // update block data
    if ( m_blockOffset == m_blockLength ) {
        m_blockAddress = m_device->Tell();
        m_blockOffset  = 0;
        m_blockLength  = 0;
    }

    return numBytesRead;
}

// reads a BGZF block
bool BgzfStream::ReadBlock(void) {

    BT_ASSERT_X( m_device, "BgzfStream::ReadBlock() - trying to read from null IO device");

    // store block's starting address
    int64_t blockAddress = m_device->Tell();

    // read block header from file
    char header[Constants::BGZF_BLOCK_HEADER_LENGTH];
    int numBytesRead = m_device->Read(header, Constants::BGZF_BLOCK_HEADER_LENGTH);

    // if block header empty
    if ( numBytesRead == 0 ) {
        m_blockLength = 0;
        return true;
    }

    // if block header invalid size
    if ( numBytesRead != Constants::BGZF_BLOCK_HEADER_LENGTH ) {
        fprintf(stderr, "BgzfStream ERROR: read block failed - could not read block header\n");
        return false;
    }

    // validate block header contents
    if ( !BgzfStream::CheckBlockHeader(header) ) {
        fprintf(stderr, "BgzfStream ERROR: read block failed - invalid block header\n");
        return false;
    }

    // copy header contents to compressed buffer
    int blockLength = BamTools::UnpackUnsignedShort(&header[16]) + 1;
    char* compressedBlock = m_compressedBlock;
    memcpy(compressedBlock, header, Constants::BGZF_BLOCK_HEADER_LENGTH);
    int remaining = blockLength - Constants::BGZF_BLOCK_HEADER_LENGTH;

    // read remainder of block
    numBytesRead = m_device->Read(&compressedBlock[Constants::BGZF_BLOCK_HEADER_LENGTH], remaining);
    if ( numBytesRead != remaining ) {
        fprintf(stderr, "BgzfStream ERROR: read block failed - could not read data from block\n");
        return false;
    }

    // decompress block data
    numBytesRead = InflateBlock(blockLength);
    if ( numBytesRead < 0 ) {
        fprintf(stderr, "BgzfStream ERROR: read block failed - could not decompress block data\n");
        return false;
    }

    // update block data
    if ( m_blockLength != 0 )
        m_blockOffset = 0;
    m_blockAddress = blockAddress;
    m_blockLength  = numBytesRead;

    // return success
    return true;
}

// seek to position in BGZF file
bool BgzfStream::Seek(const int64_t& position) {

    BT_ASSERT_X( m_device, "BgzfStream::Seek() - trying to seek on null IO device");

    // skip if not open or not seek-able
    if ( !IsOpen() || !m_device->IsRandomAccess() )
        return false;

    // determine adjusted offset & address
    int     blockOffset  = (position & 0xFFFF);
    int64_t blockAddress = (position >> 16) & 0xFFFFFFFFFFFFLL;

    // attempt seek in file
    if ( !m_device->Seek(blockAddress) ) {
        fprintf(stderr, "BgzfStream ERROR: unable to seek in file\n");
        return false;
    }

    // update block data & return success
    m_blockLength  = 0;
    m_blockAddress = blockAddress;
    m_blockOffset  = blockOffset;
    return true;
}

void BgzfStream::SetWriteCompressed(bool ok) {
    m_isWriteCompressed = ok;
}

// get file position in BGZF file
int64_t BgzfStream::Tell(void) const {
    if ( !m_isOpen ) return 0;
    return ( (m_blockAddress << 16) | (m_blockOffset & 0xFFFF) );
}

// writes the supplied data into the BGZF buffer
unsigned int BgzfStream::Write(const char* data, const unsigned int dataLength) {

    BT_ASSERT_X( m_device, "BgzfStream::Write() - trying to write to null IO device");
    BT_ASSERT_X( (m_device->Mode() == IBamIODevice::WriteOnly),
                 "BgzfStream::Write() - trying to write to non-writable IO device");

    // write blocks as needed til all data is written
    unsigned int numBytesWritten = 0;
    const char* input = data;
    unsigned int blockLength = m_uncompressedBlockSize;
    while ( numBytesWritten < dataLength ) {

        // copy data contents to uncompressed output buffer
        unsigned int copyLength = min(blockLength - m_blockOffset, dataLength - numBytesWritten);
        char* buffer = m_uncompressedBlock;
        memcpy(buffer + m_blockOffset, input, copyLength);

        // update counter
        m_blockOffset   += copyLength;
        input           += copyLength;
        numBytesWritten += copyLength;

        // flush (& compress) output buffer when full
        if ( m_blockOffset == blockLength )
            FlushBlock();
    }

    // return result
    return numBytesWritten;
}
