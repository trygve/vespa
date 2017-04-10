// Copyright 2016 Yahoo Inc. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
#pragma once

#include <memory>
#include <string>
#include "destinationsessionparams.h"
#include "imessagehandler.h"
#include "reply.h"

namespace mbus {

class MessageBus;

/**
 * A DestinationSession is used to receive Message objects and reply
 * with Reply objects.
 */
class DestinationSession : public IMessageHandler {
private:
    friend class MessageBus;

    MessageBus      &_mbus;
    string           _name;
    IMessageHandler &_msgHandler;

    /**
     * This constructor is package private since only MessageBus is supposed to
     * instantiate it.
     *
     * @param mbus    The message bus that created this instance.
     * @param params  The parameter object for this session.
     */
    DestinationSession(MessageBus &mbus, const DestinationSessionParams &params);

public:
    /**
     * Convenience typedef for an auto pointer to a DestinationSession object.
     */
    typedef std::unique_ptr<DestinationSession> UP;

    /**
     * The destructor untangles from messagebus. After this method returns,
     * messagebus will not invoke any handlers associated with this session.
     */
    virtual ~DestinationSession();

    /**
     * This method unregisters this session from message bus, effectively
     * disabling any more messages from being delivered to the message
     * handler. After unregistering, this method calls {@link
     * com.yahoo.messagebus.MessageBus#sync()} as to ensure that there are no
     * threads currently entangled in the handler.
     *
     * This method will deadlock if you call it from the message handler.
     */
    void close();

    /**
     * Convenience method used to acknowledge a Message. This method will create
     * an EmptyReply object, transfer the state from the Message to it and
     * invoke the reply method in this object.
     *
     * @param msg the Message you want to acknowledge
     */
    void acknowledge(Message::UP msg);

    /**
     * Send a Reply as a response to a Message. The Reply will be routed back to
     * where the Message came from. For this to work, it is important that the
     * messagebus state is transferred from the Message (you want to reply to)
     * to the Reply (you want to reply with). This is done with the
     * Routable::transferState method.
     *
     * @param reply the Reply
     */
    void reply(Reply::UP reply);

    /**
     * Handle a Message obtained from messagebus.
     *
     * @param message the Message
     */
    void handleMessage(Message::UP message) override;

    /**
     * Returns the message handler of this session.
     *
     * @return The message handler.
     */
    IMessageHandler &getMessageHandler() { return _msgHandler; }

    /**
     * Returns the connection spec string for this session. This returns a
     * combination of the owning message bus' own spec string and the name of
     * this session.
     *
     * @return The connection string.
     */
    const string getConnectionSpec() const;
};

} // namespace mbus

