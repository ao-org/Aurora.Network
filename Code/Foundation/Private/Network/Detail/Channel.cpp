// -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
// Copyright (C) 2018-2022 by Agustin Alvarez. All rights reserved.
//
// This work is licensed under the terms of the Apache 2.0 license.
//
// For a copy, see <https://github.com/Wolftein/Aurora.Network/blob/main/LICENSE>.
// -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

// -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
// [  HEADER  ]
// -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

#include "Channel.hpp"

// -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
// [   CODE   ]
// -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

namespace Aurora::Network::Detail
{
    constexpr static UInt32 k_Header = 2;

    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    Channel::Channel(asio::ip::tcp::socket && Channel)
        : mID      { 0 },
          mChannel { eastl::move(Channel) },
          mState   { State::Closed },
          mStats   { 0 },
          mEncoder { 65'536 },
          mDecoder { 65'536 },
          mWriter  { 0 },
          mHeader  { 0 }
    {
    }

    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    void Channel::SetID(UInt32 ID)
    {
        mID = ID;
    }

    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    UInt32 Channel::GetID() const
    {
        return mID;
    }

    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    Str8 Channel::GetAddress() const
    {
        return mChannel.remote_endpoint().address().to_v4().to_string().c_str();
    }

    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    Statistics Channel::GetStatistics() const
    {
        return mStats;
    }

    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    void Channel::Attach(OnAttach OnAttach, OnDetach OnDetach, OnForward OnForward, OnReceive OnReceive)
    {
        mOnAttach  = eastl::move(OnAttach);
        mOnDetach  = eastl::move(OnDetach);
        mOnForward = eastl::move(OnForward);
        mOnReceive = eastl::move(OnReceive);
    }

    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    void Channel::Connect(CStr8 Address, CStr8 Service)
    {
        if (mState != State::Closed)
        {
            return;
        }

        mState = State::Connecting;

        const SPtr<asio::ip::tcp::resolver> Resolver
            = eastl::make_unique<asio::ip::tcp::resolver>(mChannel.get_executor());

        const auto OnCompletion = [Self = shared_from_this(), Resolver](const auto Error, auto Result) {
            Self->WhenResolve(Error, Result);
        };
        Resolver->async_resolve(Address.data(), Service.data(), OnCompletion);
    }

    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    void Channel::Close(Bool Force)
    {
        if (mState == State::Connecting || mState == State::Connected)
        {
            if (Force || (mWriter == 0 && mEncoder.IsEmpty()))
            {
                DoClose(0);
            }
            else
            {
                Flush();

                mState = State::Closing;
            }
        }
    }

    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    void Channel::Send(Bool Urgent, Writer & Message)
    {
        if (mState == State::Connected && Message.GetOffset() > 0)
        {
            const CPtr<UInt08> Chunk = mEncoder.Reserve(Message.GetOffset() + k_Header);

            if (Chunk.empty())
            {
                DoClose(0);
            }
            else
            {
                const CPtr<const UInt08> Block = Message.GetData();

                * reinterpret_cast<UInt16 *>(Chunk.data()) = Block.size();

                eastl::copy(Block.begin(), Block.begin() + Message.GetOffset(), Chunk.data() + k_Header);

                mOnForward(shared_from_this(), Chunk.subspan(k_Header, Message.GetOffset()));

                mEncoder.Commit(Chunk.size());

                ++mStats.TotalPacketSent;
                mStats.TotalBytesPending += Block.size() + k_Header;

                if (Urgent)
                {
                    Flush();
                }
            }
        }
    }

    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    void Channel::Flush()
    {
        if (mState == State::Connected && mWriter == 0)
        {
            DoFlush();
        }
    }

    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    void Channel::Start()
    {
        mState = State::Connected;

        DoConfiguration();
        {
            mOnAttach(shared_from_this());
        }
        DoRead(Sequence::Header, sizeof(mHeader));
    }

    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    void Channel::DoClose(SInt32 Code)
    {
        if (mState == State::Closed)
        {
            return;
        }

        mState = State::Closed;
        mStats = { 0 };

        asio::error_code Error;
        mChannel.shutdown(asio::ip::tcp::socket::shutdown_both, Error);
        mChannel.close(Error);

        mWriter = 0;
        mEncoder.Reset();
        mDecoder.Reset();

        mOnDetach(shared_from_this(), Code);
    }

    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    void Channel::DoConfiguration()
    {
        mChannel.set_option(asio::ip::tcp::no_delay(true));
    }

    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    void Channel::DoRead(Sequence Operation, UInt32 Quantity)
    {
        if (Operation == Sequence::Header)
        {
            const auto OnCompletion = [Self = shared_from_this()](const auto Error, auto Transferred) {
                Self->WhenRead(Error, Transferred, Sequence::Header);
            };
            asio::async_read(mChannel, asio::mutable_buffer(& mHeader, k_Header), OnCompletion);
        }
        else
        {
            CPtr<UInt08> Chunk = mDecoder.Reserve(Quantity);

            if (Chunk.empty())
            {
                DoClose(0);
            }
            else
            {
                const auto OnCompletion = [Self = shared_from_this()](const auto Error, auto Transferred) {
                    Self->WhenRead(Error, Transferred, Sequence::Body);
                };
                asio::async_read(mChannel, asio::mutable_buffer(Chunk.data(), Chunk.size()), OnCompletion);
            }
        }
    }

    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    void Channel::DoFlush()
    {
        if (const CPtr<UInt08> Chunk = mEncoder.Read(); !Chunk.empty())
        {
            mWriter = Chunk.size();

            const auto OnCompletion = [Self = shared_from_this()](const auto Error, auto Transferred) {
                Self->WhenWrite(Error, Transferred);
            };
            asio::async_write(mChannel, asio::const_buffer(Chunk.data(), Chunk.size()), OnCompletion);
        }
        else
        {
            mWriter = 0;

            if (mState == State::Closing)
            {
                DoClose(0);
            }
        }
    }

    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    void Channel::DoProcess()
    {
        const CPtr<UInt08> Chunk = mDecoder.Read();

        if (!Chunk.empty())
        {
            ++mStats.TotalPacketReceived;

            mOnReceive(shared_from_this(), Chunk);
        }
        mDecoder.Consume(Chunk.size());
    }

    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    void Channel::WhenResolve(const std::error_code& Error,
        asio::ip::tcp::resolver::results_type Result)
    {
        if (Error)
        {
            WhenError(Error);
            return;
        }

        auto Self = shared_from_this();

        // Let ASIO try all endpoints in 'Result' for us.
        asio::async_connect(
            mChannel,
            Result,
            [Self, Result](const std::error_code& ec, const auto&) mutable
            {
                // Signature unchanged; we still pass a results_type.
                Self->WhenConnect(ec, Result);
            });
    }

    void Channel::WhenConnect(const std::error_code& Error,
        asio::ip::tcp::resolver::results_type /*Result*/)
    {
        if (Error)
        {
            mChannel.close();
            // async_connect already tried all endpoints in the sequence.
            WhenError(Error);
            return;
        }

        Start();
    }


    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    void Channel::WhenError(const std::error_code & Error)
    {
        DoClose(Error.value());
    }

    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    void Channel::WhenRead(const std::error_code & Error, UInt32 Transferred, Sequence Operation)
    {
        if (Error)
        {
            WhenError(Error);
        }
        else
        {
            if (mState != State::Connected)
            {
                return;
            }

            mStats.TotalBytesReceived += Transferred;

            if (Operation == Sequence::Header)
            {
                DoRead(Sequence::Body, mHeader);
            }
            else
            {
                mDecoder.Commit(Transferred);

                DoProcess();

                DoRead(Sequence::Header, sizeof(mHeader));
            }
        }
    }

    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    void Channel::WhenWrite(const std::error_code & Error, UInt32 Transferred)
    {
        if (mState == State::Closed)
        {
            return;
        }

        if (Error)
        {
            WhenError(Error);
        }
        else
        {
            mStats.TotalBytesSent    += Transferred;
            mStats.TotalBytesPending -= Transferred;

            mEncoder.Consume(mWriter);

            DoFlush();
        }
    }
}