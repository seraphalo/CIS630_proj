
#define packed __attribute__((packed))

#define MSG_MAX 256
typedef int msg_t;

#define MSG_TEXT 0
#define MSG_PROBE 10
#define MSG_PROBE_ACK 11
#define MSG_PROBE_NAK 12
#define MSG_ELECTION 20
#define MSG_ELECTED 21
#define MSG_TOKEN 30
#define MSG_RECOVER 40

struct msg{
    msg_t msg_type; // just an int
}packed;

struct  msg_text{
    msg_t msg_type;
    int sourceID;
    int seqNum;
    char post[MSG_MAX];
}packed;

struct msg_probe{
	msg_t msg_type;
	int sourceID;
	int msgID;
}packed;

struct msg_probe_ack{
	msg_t msg_type;
	int sourceID;
	int msgID;
}packed;

struct msg_probe_nak{
	msg_t msg_type;
	int sourceID;
	int msgID;
}packed;

struct msg_election{
	msg_t msg_type;
	int clientID;
	int electionID;
	int bestCand;
}packed;

struct msg_elected{
	msg_t msg_type;
	int clientID;
	int electionID;
	int electedID;
}packed;

struct msg_token{
	msg_t msg_type;
	int clientID;
	int tokenID;
}packed;

struct msg_recover{
	msg_t msg_type;
	int sourceID;
}packed;









