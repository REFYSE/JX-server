# JX-server
A networked server (“JXServer”) that sends files to clients in response to requests. Supports multiple connecting clients simultaneously as well as multiple simultaneous connections from the same client for increased transfer speeds.

It will accept exactly 1 command line argument as follows:

```./server <configuration file>```

The argument <configuration file> will be a path to a binary configuration file. The data of this file must be arranged in the following layout from the start:
  
* 4 bytes - the IPv4 address your server will listen on, in network byte order
* 2 bytes - representing the TCP port number your server will listen on, in network byte order
* All remaining bytes until the end of the file - ASCII representing the relative or absolute path to the directory (“the target directory”), from which the server will offer files to clients. This is not NULL terminated.

The server will listen for and accept connections from clients. Upon a successful connection, clients will send a number of possible requests, described in the below format. 
  
All client requests and server responses consist of one or more structured series of bytes, called messages. Each message will contain the following fields in the below format:
* 1 byte - Message header; this describes details about the message as follows.
    * First 4 bits - “Type digit”: A single hexadecimal digit that defines the type of request or response. It is unique for different types of messages.
    * 5th bit - “Compression bit”: If this bit is 1, it indicates that the payload is compressed. Otherwise, the payload is to be read as is.
    * 6th bit - “Requires compression bit”: If this bit is 1 in a request message, it indicates that all server responses (except for error responses) must be compressed. If it is 0, server response compression is optional. It has no meaning in a
response message.
    * 7th and 8th bits - padding bits, which should all be 0.
* 8 bytes - Payload length; unsigned integer representing the length of the variable payload in
bytes (in network byte order).
* Variable byte payload - a sequence of bytes, with length equal to the length field described
previously. It will have different meanings depending on the type of request/response message.
  
### Echo
Clients may request an “echo”. The request type digit is 0x0.
 
### Direct Listing
The request type is 0x2. There must be no payload and the payload length field should be 0. The client is requesting a list of all files in the server’s target directory. The server will send back a response with type 0x3. The payload will contain the filenames of every regular file in the target directory provided in the command line arguments to the server. These filenames are sent end to end in the payload, separated by NULL (0x00) byte with a NULL (0x00) byte at the end of the payload.
  
### File Size Query
The request type is 0x4. The request payload must be a NULL terminated string that represents a target filename, for which the client is requesting the size. The server will send back a response with type 0x5. The payload will contain the length of the file with the target filename in the target directory,
in bytes, represented as a 8-byte unsigned integer in network byte order. If the requested filename does not exist, an error response is returned.
 
### Retrieve File
The request type is 0x6. This is a request for part or whole of a file in the server’s target directory. The payload must consist of the following structure:
* 4 bytes - an arbitrary sequence of bytes that represents a session ID for this request.
* 8 bytes - the starting offset of data, in bytes, that should be retrieved from the file
* 8 bytes - the length of data, in bytes, that should be retrieved from the file
* Variable bytes - a NULL terminated string that represents a target filename

In response to this request, the server sends back one or more response messages with type 0x7. Each response of type 0x7 represents a portion of the requested file data. Each payload will consist of the following structure:
* 4 bytes - the same session ID as was provided in the original request
* 8 bytes - a starting offset of data, in bytes, from the target file, that this response contains
* 8 bytes - the length of data, in bytes, from the target file, that this response contains
* Variable bytes - the actual data from the target file at the declared offset and length in this response

The client may open several concurrent requests for the same filename on different simultaneous connections, with the same session ID. If multiple connections with requests for the same file range with the same session ID is received, the server will multiplex the file data across those connections. The client may make an extra concurrent connection for a given file at any time.

### Shutdown
The request type digit is 0x8 and the payload must be 0 length.

### Losssless compression
For any message where the compression bit is set in the message header, the variable length payload is assumed to be losslessly compressed. Responses that require compression will apply compression by replacing bytes of uncompressed data with variable length bit sequences (“bit codes”). A compression dictionary defines the mapping of bytes to bit codes and consists of 256 segments of variable length. Each segment corresponds in order to input byte values from 0x00 to 0xFF. Each segment may not necessarily be aligned to a byte boundary. However, at the end of the 256 segments, there must be padding with 0 bits to the next byte boundary. This means the entire compression dictionary is aligned to a byte boundary overall. The overall structure follows:
* 256 of the following segments, with no padding in between (including no padding to byte boundaries):
    * 1 byte - unsigned integer representing the length of this code in bits; this is equal to the length of the next field in bits. It is not necessarily aligned to a byte boundary.
    * Variable number of bits - the bit code that is used to encode the byte value corresponding to this segment. It is not necessarily aligned to a byte boundary.
    * Variable number of bits - padding with 0 bits to the next byte boundary. This ensures that the entire dictionary is aligned to a byte boundary.
  
A binary file that contains a compression dictionary in the structure described above must be provided. This binary file must be called ```compression.dict``` and be present in the same directory as the server executable.
