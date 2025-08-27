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

#include "Acceptor.hpp"

// -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
// [   CODE   ]
// -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

namespace Aurora::Network::Detail
{
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    Acceptor::Acceptor()
        : mAcceptor  { mContext },
          mConnector { mContext }
    {
    }

    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    Acceptor::~Acceptor()
    {
        Close();
    }

    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    void Acceptor::Poll()
    {
        mContext.poll();
    }

    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    void Acceptor::Attach(OnAttach OnAttach, OnDetach OnDetach, OnForward OnForward, OnReceive OnReceive, OnError OnError)
    {
        const auto OnChannelAttach  = [OnAttach](const SPtr<Channel> & Channel) {
            if (OnAttach) OnAttach(Channel->GetID(), Channel->GetAddress());
        };
        const auto OnChannelDetach  = [this, OnDetach](const SPtr<Channel> & Channel, SInt32 Code) {
            if (OnDetach) OnDetach(Channel->GetID());

            WhenDetach(Channel);
        };
        const auto OnChannelForward = [OnForward](const SPtr<Channel> & Channel, CPtr<UInt08> Data) {
            Reader Message(Data);
            if (OnForward) OnForward(Channel->GetID(), Message);
        };
        const auto OnChannelReceive = [OnReceive](const SPtr<Channel> & Channel, CPtr<UInt08> Data) {
            Reader Message(Data);
            if (OnReceive) OnReceive(Channel->GetID(), Message);
        };
        const auto OnChannelError = [OnError](UInt32 Code, CStr8 Description) {
            if (OnError) OnError(Code, Description);
        };

        mOnAttach  = OnChannelAttach;
        mOnDetach  = OnChannelDetach;
        mOnForward = OnChannelForward;
        mOnReceive = OnChannelReceive;
        mOnError   = OnChannelError;
    }

    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    Bool Acceptor::Listen(UInt32 Capacity, CStr8 Address, CStr8 Service)
    {
        // Resolve address -> endpoints (modern ASIO)
        asio::ip::tcp::resolver resolver(mAcceptor.get_executor());
        asio::error_code InError;
        auto results = resolver.resolve(Address.data(), Service.data(), InError);
        if (InError || results.begin() == results.end())
        {
            const auto msg = InError ? InError.message() : std::string("resolve returned no endpoints");
            mOnError(InError ? InError.value() : static_cast<int>(asio::error::not_found),
                MakeStringCompatible(msg));
            return false;
        }

        const asio::ip::tcp::endpoint Endpoint = results.begin()->endpoint();

        mDatabase.resize(Capacity + 1);

        // Open
        InError.clear();
        mAcceptor.open(Endpoint.protocol(), InError);
        if (InError)
        {
            mOnError(InError.value(), MakeStringCompatible(InError.message()));
            return false;
        }

        // Reuse address
        mAcceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true), InError);
        if (InError)
        {
            mOnError(InError.value(), MakeStringCompatible(InError.message()));
            return false;
        }

        // Bind
        mAcceptor.bind(Endpoint, InError);
        if (InError)
        {
            mOnError(InError.value(), MakeStringCompatible(InError.message()));
            return false;
        }

        // Listen
        mAcceptor.listen(static_cast<int>(mDatabase.size()), InError);
        if (InError)
        {
            mOnError(InError.value(), MakeStringCompatible(InError.message()));
            return false;
        }

        const auto OnCompletion = [Self = this](const auto Error) {
            Self->WhenAccept(Error);
            };
        mAcceptor.async_accept(mConnector, OnCompletion);

        return true;
    }



    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    void Acceptor::Close()
    {
        mAcceptor.close();
    }

    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    void Acceptor::Kick(UInt32 ID, Bool Force)
    {
        if (const SPtr<Channel> & Channel = mDatabase[ID])
        {
            Channel->Close(Force);
        }
    }

    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    void Acceptor::Send(UInt32 ID, Bool Urgent, Writer & Message)
    {
        if (const SPtr<Channel> & Channel = mDatabase[ID])
        {
            Channel->Send(Urgent, Message);
        }
    }

    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    void Acceptor::Broadcast(CPtr<UInt32> Group, Bool Urgent, Writer & Message)
    {
        for (const UInt32 Session : Group)
        {
            Send(Session, Urgent, Message);
        }
    }

    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    void Acceptor::Flush()
    {
        for (const SPtr<Channel> & Session : mActive)
        {
            Session->Flush();
        }
    }

    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    void Acceptor::Flush(UInt32 ID)
    {
        if (const SPtr<Channel> & Session = mDatabase[ID])
        {
            Session->Flush();
        }
    }

    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    Statistics Acceptor::GetStatistics(UInt32 ID) const
    {
        if (const SPtr<Channel> & Session = mDatabase[ID])
        {
            return Session->GetStatistics();
        }
        return { 0 };
    }

    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    void Acceptor::WhenAccept(const std::error_code & Error)
    {
        if (Error)
        {
            Close();
        }
        else
        {
            SPtr<Channel> Connection = eastl::make_shared<Channel>(eastl::move(mConnector));

            if (WhenAttach(Connection))
            {
                mDatabase[Connection->GetID()] = Connection;

                Connection->Attach(mOnAttach, mOnDetach, mOnForward, mOnReceive);
                Connection->Start();
            }

            const auto OnCompletion = [Self = this](const auto Error) {
                Self->WhenAccept(Error);
            };
            mAcceptor.async_accept(mConnector = asio::ip::tcp::socket(mAcceptor.get_executor()), OnCompletion);
        }
    }

    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    Bool Acceptor::WhenAttach(const SPtr<Channel> & Channel)
    {
        for (UInt32 Index = 1; Index < mDatabase.size(); ++Index)
        {
            if (mDatabase[Index] == nullptr)
            {
                mActive.emplace_back(Channel);
                Channel->SetID(Index);
                return true;
            }
        }
        return false;
    }

    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
    // -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

    void Acceptor::WhenDetach(const SPtr<Channel> & Channel)
    {
        mActive.erase_first(Channel);

        mDatabase[Channel->GetID()] = nullptr;
    }
}