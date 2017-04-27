//-----------------------------------------------------------------------------
// Class:	WebSocketReader
// Authors:	leio
// Date:	2017/4/26
// Desc:  Parse WebSocket protocol
//-----------------------------------------------------------------------------
#include "WebSocketReader.h"
#include <vector>
using namespace NPL::WebSocket;


void WebSocketReader::assertSanePayloadLength(int length)
{

}

WebSocketReader::WebSocketReader()
	: state(START)
	, cursor(0)
	, flagsInUse(0x00)
{
	frame = new WebSocketFrame();
}

WebSocketReader::~WebSocketReader()
{
	delete frame;
}

ByteBuffer WebSocketReader::load(Buffer_Type* buffer, int bytes_transferred)
{
	ByteBuffer b;
	if (!buffer)
	{
		return b;
	}
	Buffer_Type::iterator curIt = buffer->begin();
	Buffer_Type::iterator curEnd = buffer->begin() + bytes_transferred;
	while (curIt != curEnd)
	{
		b.putChar(*curIt);
		curIt++;
	}

	return b;
}
void WebSocketReader::parse(ByteBuffer& buffer)
{
	if (buffer.bytesRemaining() <= 0)
	{
		return;
	}
	while (parseFrame(buffer))
	{

	}
}
bool WebSocketReader::parseFrame(ByteBuffer& buffer)
{
	while (buffer.bytesRemaining() > 0)
	{
		switch (state)
		{
		case NPL::WebSocket::State::START:
		{
			byte b = buffer.get();
			bool fin = ((b & 0x80) != 0);
			byte opcode = (byte)(b & 0x0F);
			if (!WebSocketCommon::isKnown(opcode))
			{
				return false;
			}
			switch (opcode)
			{
			case NPL::WebSocket::OpCode::TEXT:
			case NPL::WebSocket::OpCode::BINARY:
			case NPL::WebSocket::OpCode::CLOSE:
			case NPL::WebSocket::OpCode::PING:
			case NPL::WebSocket::OpCode::PONG:
			{
				frame->reset();
				frame->setOpCode(opcode);
				break;
			}
			}
			frame->setFin(fin);
			// Are any flags set?
			if ((b & 0x70) != 0)
			{
				/*
				* RFC 6455 Section 5.2
				*
				* MUST be 0 unless an extension is negotiated that defines meanings for non-zero values. If a nonzero value is received and none of the
				* negotiated extensions defines the meaning of such a nonzero value, the receiving endpoint MUST _Fail the WebSocket Connection_.
				*/
				if ((b & 0x40) != 0)
				{
					if (isRsv1InUse())
						frame->setRsv1(true);
				}
				if ((b & 0x20) != 0)
				{
					if (isRsv2InUse())
						frame->setRsv2(true);
				}
				if ((b & 0x10) != 0)
				{
					if (isRsv3InUse())
						frame->setRsv3(true);
				}
			}
			state = PAYLOAD_LEN;
			break;
		}
			
		case NPL::WebSocket::State::PAYLOAD_LEN:
		{
			byte b = buffer.get();
			frame->setMasked((b & 0x80) != 0);
			payloadLength = (byte)(0x7F & b);

			if (payloadLength == 127) // 0x7F
			{
				// length 8 bytes (extended payload length)
				payloadLength = 0;
				state = PAYLOAD_LEN_BYTES;
				cursor = 8;
				break; // continue onto next state
			}
			else if (payloadLength == 126) // 0x7E
			{
				// length 2 bytes (extended payload length)
				payloadLength = 0;
				state = PAYLOAD_LEN_BYTES;
				cursor = 2;
				break; // continue onto next state
			}
			assertSanePayloadLength(payloadLength);
			if (frame->isMasked())
			{
				state = MASK;
			}
			else
			{
				// special case for empty payloads (no more bytes left in buffer)
				if (payloadLength == 0)
				{
					state = START;
					return true;
				}
				// what is maskProcessor?
				//maskProcessor.reset(frame);
				state = PAYLOAD;
			}
			break;
		}
			
		case NPL::WebSocket::State::PAYLOAD_LEN_BYTES:
		{
			byte b = buffer.get();
			--cursor;
			payloadLength |= (b & 0xFF) << (8 * cursor);
			if (cursor == 0)
			{
				assertSanePayloadLength(payloadLength);
				if (frame->isMasked())
				{
					state = MASK;
				}
				else
				{
					// special case for empty payloads (no more bytes left in buffer)
					if (payloadLength == 0)
					{
						state = START;
						return true;
					}

					// what is maskProcessor?
					//maskProcessor.reset(frame);
					state = PAYLOAD;
				}
			}
			break;
		}
		case NPL::WebSocket::State::MASK:
		{
			byte m[4];
			std::vector<byte> mm(m,m+4);
			frame->setMask(mm);
			if (buffer.bytesRemaining() >= 4)
			{
				buffer.getBytes(m, 4);
				// special case for empty payloads (no more bytes left in buffer)
				if (payloadLength == 0)
				{
					state = START;
					return true;
				}
				// what is maskProcessor?
				//maskProcessor.reset(frame);
				state = PAYLOAD;
			}
			else
			{
				state = MASK_BYTES;
				cursor = 4;
			}
			break;
		}
		case NPL::WebSocket::State::MASK_BYTES:
		{
			byte b = buffer.get();
			std::vector<byte> mask = frame->getMask();
			mask[4 - cursor] = b;
			frame->setMask(mask);
			--cursor;
			if (cursor == 0)
			{
				// special case for empty payloads (no more bytes left in buffer)
				if (payloadLength == 0)
				{
					state = START;
					return true;
				}
				//// what is maskProcessor?
				//maskProcessor.reset(frame);
				state = PAYLOAD;
			}
			break;
		}
		case NPL::WebSocket::State::PAYLOAD:
		{
			frame->assertValid();
			if (parsePayload(buffer))
			{
				// special check for close
				if (frame->getOpCode() == OpCode::CLOSE)
				{
					// TODO: yuck. Don't create an object to do validation checks!
					// new CloseInfo(frame);
				}
				state = START;
				// we have a frame!
				return true;
			}
			break;
		}
		}
	}
	return true;
}

bool WebSocketReader::parsePayload(ByteBuffer& buffer)
{
	if (payloadLength == 0)
	{
		return true;
	}

	const int len = buffer.bytesRemaining();
	if ( len > 0)
	{

		byte b[1024];
		buffer.getBytes(b, len);
		// Create a small window of the incoming buffer to work with.
		// this should only show the payload itself, and not any more
		// bytes that could belong to the start of the next frame.
		//int bytesSoFar = payload == null ? 0 : payload.position();
		//int bytesExpected = payloadLength - bytesSoFar;
		//int bytesAvailable = buffer.remaining();
		//int windowBytes = Math.min(bytesAvailable, bytesExpected);
		//int limit = buffer.limit();
		//buffer.limit(buffer.position() + windowBytes);
		//ByteBuffer window = buffer.slice();
		//buffer.limit(limit);
		//buffer.position(buffer.position() + window.remaining());

		//if (LOG.isDebugEnabled()) {
		//	LOG.debug("{} Window: {}", policy.getBehavior(), BufferUtil.toDetailString(window));
		//}

		//maskProcessor.process(window);

		//if (window.remaining() == payloadLength)
		//{
		//	// We have the whole content, no need to copy.
		//	frame.setPayload(window);
		//	return true;
		//}
		//else
		//{
		//	if (payload == null)
		//	{
		//		payload = bufferPool.acquire(payloadLength, false);
		//		BufferUtil.clearToFill(payload);
		//	}
		//	// Copy the payload.
		//	payload.put(window);

		//	if (payload.position() == payloadLength)
		//	{
		//		BufferUtil.flipToFlush(payload, 0);
		//		frame.setPayload(payload);
		//		return true;
		//	}
		//}
	}
	return false;
}

