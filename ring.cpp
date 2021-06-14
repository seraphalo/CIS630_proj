#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <fcntl.h>
#include <iostream>
#include <time.h>
#include <string>
#include <map>
#include <chrono>
#include <fstream>
#include <string>
#include "ring.h"

using namespace std;


template <typename V, typename R>
ostream& operator << (ostream& s, const chrono::duration<V,R>& d){
    int seconds = d.count() * R::num / R::den;
    int mins = seconds / 60;
    s << mins << ":" << seconds%60;
    return s;
}


int handle_client_input();
int send_probe();
int handle_probe(struct msg* msg);
int send_probe_ack(struct msg_probe* probe_msg);
int handle_probe_ack(struct msg* recv_msg);
int send_probe_nak(struct msg_probe* probe_msg);
int handle_probe_nak(struct msg* recv_msg);
int start_election();
int handle_election(struct msg* recv_msg);
int forward_election(struct msg_election* recv_msg);
int send_elected(struct msg_elected* recv_elected);
int handle_elected(struct msg* recv_msg);
int send_token();
int handle_token(struct msg* recv_msg);
int send_text();
int handle_text(struct msg* recv_msg, struct sockaddr_in sock);
int send_recover(int sourceID);
int handle_recover(struct msg* recv_msg);



int minPort, maxPort;
int myPort;
char hostname[10] = "localhost";
int s;  // socket
struct sockaddr_in next_addr;
struct sockaddr_in my_addr;
struct sockaddr_in sending_addr;
int nextPort = -1;
int prevPort = -1;
int seq = 0;
int cont;
int currentProbe;
int startElection = 0;
int leader;
int sending = 0;
int tokenExist = 0;
int recovering = 0;
struct msg_token token;
ofstream output;
ifstream input;
chrono::system_clock::time_point lastMSG;
chrono::system_clock::time_point lastToken;
chrono::system_clock::duration timeout;
chrono::seconds recoverInterval(2);
chrono::system_clock::time_point nextFound;
chrono::system_clock::time_point start;
chrono::system_clock::time_point sentTime;
chrono::system_clock::time_point lastRecover;


int main(int argc, char const *argv[])
{
    if(argc != 7 ||
        argv[1][0] != '-' || argv[1][1] != 'c' ||
        argv[3][0] != '-' || argv[3][1] != 'i' ||
        argv[5][0] != '-' || argv[5][1] != 'o'){
        fprintf(stderr, "Usage: ring -c cfg.txt -i in.txt -o out.txt\n");
        exit(-1);
    }

    FILE* cfg = fopen(argv[2], "r");
    if(cfg == NULL){
        fprintf(stderr, "cannot open file: %s\n", argv[2]);
        exit(-2);
    }

    input.open(argv[4]);
    output.open(argv[6]);

    struct timeval joinTime;
    chrono::seconds leaveTime;
    int waitElectionMS = rand() % 1000;
    chrono::milliseconds waitElection(waitElectionMS);

    char cfgLine[50];
    char* reading;
    while((reading = fgets(cfgLine, 50, cfg)) != NULL){
        // splitting by space
        char* prt = strtok(cfgLine, " ");
        if(strncmp(prt, "client_port:", 12) == 0){
            prt = strtok(NULL, " ");
            char* portMin = strtok(prt, "-");
            char* portMax = strtok(NULL, "\n");
            minPort = atoi(portMin);
            maxPort = atoi(portMax);
        }else if(strncmp(prt, "my_port:", 8) == 0){
            prt = strtok(NULL, "\n");
            myPort = atoi(prt);
            output << "Port number " << myPort << endl << endl;
        }else if(strncmp(prt, "join_time:", 10) == 0){
            prt = strtok(NULL, "\n");
            char* min = strtok(prt, ":");
            char* sec = strtok(NULL, "\n");
            joinTime.tv_sec = atoi(min) * 60 + atoi(sec);
            joinTime.tv_usec = 0;
        }else if(strncmp(prt, "leave_time:", 12) == 0){
            prt = strtok(NULL, "\n");
            char* min = strtok(prt, ":");
            char* sec = strtok(NULL, "\n");
            leaveTime = chrono::seconds(atoi(min) * 60 + atoi(sec));
        }
    }

    s = socket(PF_INET, SOCK_DGRAM, 0);
    if (s < 0){
        fprintf(stderr, "socket failed for port: %d\n", myPort);
        exit(-3);
    }


    next_addr.sin_family = AF_INET;
    my_addr.sin_family = AF_INET;
    sending_addr.sin_family = AF_INET;

    my_addr.sin_port = htons(myPort);

    struct hostent *he;
    if ((he = gethostbyname(hostname)) == NULL) {
        puts("error resolving hostname..");
        exit(1);
    }
    memcpy(&next_addr.sin_addr, he->h_addr_list[0], he->h_length);
    memcpy(&my_addr.sin_addr, he->h_addr_list[0], he->h_length);
    memcpy(&sending_addr.sin_addr, he->h_addr_list[0], he->h_length);

    int err = bind(s, (struct sockaddr*)&my_addr, sizeof my_addr);
    if(err < 0){
        fprintf(stderr, "bind failed for client %d\n", myPort);
        exit(-3);
    }

    cont = 1;
    timeout = chrono::seconds(5);

    start = chrono::system_clock::now();
    lastRecover = start;
    lastToken = chrono::system_clock::now();
    struct timeval halfSec;
    halfSec.tv_sec = 0;
    halfSec.tv_usec = 500000;

    int rc;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(s, &fds);

    // timeout is the join time
    rc = select(s+1, &fds, NULL, NULL, &joinTime);
    send_probe();

    while (cont)
    {
        FD_ZERO(&fds);
        FD_SET(s, &fds);
        chrono::system_clock::time_point rightNow = chrono::system_clock::now();


        if(startElection && rightNow - nextFound > waitElection)
            start_election();
 

        if((rightNow - sentTime > timeout && sending == 1) ||
            (rightNow - lastToken > timeout * 50
            && rightNow - lastRecover > recoverInterval)){
            prevPort = -1;
            nextPort = -1;
            send_probe();
            send_recover(myPort);
            sending = 0;
            tokenExist = 0;
            lastToken = chrono::system_clock::now();
            lastRecover = chrono::system_clock::now();
        }


        // every one sec, stop blocking and check for the time
        rc = select(s+1, &fds, NULL, NULL, &halfSec);
        
        if (rc < 0)
            fprintf(stderr, "error in select\n");
        else{
            if (FD_ISSET(s, &fds))
                handle_client_input();
        }

        chrono::system_clock::time_point timeCheck = chrono::system_clock::now();
        // leave the ring
        if(timeCheck - start > leaveTime){
            if(sending == 1)
                send_token();
            break;
        }
    }

    input.close();
    output.close();
    fclose(cfg);
    return 0;
}

int handle_client_input(){
    ssize_t bytes;
    void *data;
    size_t len;

    char recv_text[1024];
    data = &recv_text;
    len = sizeof recv_text;

    struct sockaddr_in recv_client;
    socklen_t fromlen = sizeof(recv_client);

    bytes = recvfrom(s, data, len, 0, (struct sockaddr*)&recv_client, &fromlen);
    if(bytes < 0){
        fprintf(stderr, "recv failed\n");
        return -1;
    }

    if (bytes < 0){
        perror ("recvfrom failed\n");
        return -1;
    }

    struct msg* recv_msg;
    recv_msg = (struct msg*)data;
    msg_t recv_msg_type = recv_msg->msg_type;



    if(recv_msg_type == MSG_PROBE)
        handle_probe(recv_msg);
    else if(recv_msg_type == MSG_PROBE_ACK)
        handle_probe_ack(recv_msg);
    else if(recv_msg_type == MSG_PROBE_NAK)
        handle_probe_nak(recv_msg);
    else if(recv_msg_type == MSG_ELECTION)
        handle_election(recv_msg);
    else if(recv_msg_type == MSG_ELECTED)
        handle_elected(recv_msg);
    else if(recv_msg_type == MSG_TOKEN)
        handle_token(recv_msg);
    else if(recv_msg_type == MSG_text)
        handle_text(recv_msg, recv_client);
    else if(recv_msg_type == MSG_RECOVER)
        handle_recover(recv_msg);
    return 0;
}

int possibleNewNext(int sourceID){
    return nextPort == -1 ||
            (sourceID > myPort && sourceID < nextPort) ||
            (nextPort < myPort && sourceID < nextPort) ||
            (nextPort < myPort && myPort < sourceID);
}


int handle_probe_ack(struct msg* recv_msg){
    lastMSG = chrono::system_clock::now();
    struct msg_probe_ack* probe_ack = (struct msg_probe_ack*)recv_msg;
    int sourceID = probe_ack->sourceID;
    // not the current ack's response
    if(probe_ack->msgID != currentProbe)
        return -1;
    // first probe ack received for current probe ID
    if(possibleNewNext(sourceID)){
        if(sourceID != nextPort){
            nextFound = chrono::system_clock::now();
            nextPort = sourceID;
            output << nextFound - start <<
                ": next hop is changed to client " << nextPort << endl;
        }
        startElection = 1;
    }
    return 0;
}

int handle_probe_nak(struct msg* recv_msg){
    lastMSG = chrono::system_clock::now();
    struct msg_probe_nak* probe_nak = (struct msg_probe_nak*)recv_msg;
    return 0;
}

int send_probe_ack(struct msg_probe* probe_msg){
    ssize_t bytes;
    void *data;
    size_t len;

    struct msg_probe_ack probe_ack;
    probe_ack.msg_type = MSG_PROBE_ACK;
    probe_ack.sourceID = myPort;
    probe_ack.msgID = probe_msg->msgID;

    data = &probe_ack;
    len = sizeof probe_ack;

    sending_addr.sin_port = htons(probe_msg->sourceID);
    bytes = sendto(s, data, len, 0, (struct sockaddr*)&sending_addr, sizeof sending_addr);
    if(bytes < 0)
        fprintf(stderr, "sending probe_ack failed\n");
    seq ++;
    return 0;
}

int send_probe_nak(struct msg_probe* probe_msg){
    ssize_t bytes;
    void *data;
    size_t len;

    struct msg_probe_nak probe_nak;
    probe_nak.msg_type = MSG_PROBE_NAK;
    probe_nak.sourceID = myPort;
    probe_nak.msgID = probe_msg->msgID;

    data = &probe_nak;
    len = sizeof probe_nak;

    sending_addr.sin_port = htons(probe_msg->sourceID);
    bytes = sendto(s, data, len, 0, (struct sockaddr*)&sending_addr, sizeof sending_addr);
    if(bytes < 0)
            fprintf(stderr, "sending probe_ack failed\n");
    seq ++;
    return 0;
}

int set_new_prevPort(int sourceID, struct msg_probe* probe_msg){
    if(prevPort != sourceID){
        prevPort = sourceID;
        output << chrono::system_clock::now() - start <<
            ": previous hop is changed to client " << prevPort << endl;
    }
    send_probe_ack(probe_msg);
    return 0;
}

int handle_probe(struct msg* recv_msg){
    lastMSG = chrono::system_clock::now();
    struct msg_probe* probe_msg = (struct msg_probe*)recv_msg;
    int sourceID = probe_msg->sourceID;
    if(possibleNewNext(sourceID) && sourceID != prevPort)
        send_probe();
    if(prevPort == -1)
        return set_new_prevPort(sourceID, probe_msg);

    if(sourceID == prevPort)
        return set_new_prevPort(sourceID, probe_msg);

    if((sourceID > prevPort && sourceID < myPort) ||
        (myPort < prevPort && sourceID < myPort) ||
        (myPort < prevPort && sourceID > prevPort))
        return set_new_prevPort(sourceID, probe_msg);
    else
        return send_probe_nak(probe_msg);
}



int send_probe(){
    ssize_t bytes;
    void *data;
    size_t len;

    struct msg_probe probe;
    probe.msg_type = MSG_PROBE;
    probe.sourceID = myPort;
    probe.msgID = seq;

    currentProbe = seq;
    // nextPort = myPort;
    startElection = 0;
    nextPort = -1;
    seq ++;

    data = &probe;
    len = sizeof probe;

    int i;
    for(i=myPort+1; i <= maxPort; i++){
        // fprintf(stderr, "sending to %d\n", i);
        next_addr.sin_port = htons(i);
        bytes = sendto(s, data, len, 0, (struct sockaddr*)&next_addr, sizeof next_addr);
        if(bytes < 0)
            fprintf(stderr, "sending to %d failed\n", i);
    }

    for(i=minPort; i < myPort; i++){
        // fprintf(stderr, "sending to %d\n", i);
        next_addr.sin_port = htons(i);
        bytes = sendto(s, data, len, 0, (struct sockaddr*)&next_addr, sizeof next_addr);
        if(bytes < 0)
            fprintf(stderr, "sending to %d failed\n", i);
    }
    return 0;
}

int start_election(){
    output << chrono::system_clock::now() - start <<
        ": started election, send election message to client " <<
        nextPort << endl;
    startElection = 0;
    ssize_t bytes;
    void *data;
    size_t len;

    struct msg_election election;
    election.msg_type = MSG_ELECTION;
    election.clientID = myPort;
    srand(time(0));
    election.electionID = rand();
    election.bestCand = myPort;

    data = &election;
    len = sizeof election;

    next_addr.sin_port = htons(nextPort);
    bytes = sendto(s, data, len, 0, (struct sockaddr*)&next_addr, sizeof next_addr);
    if(bytes < 0)
        fprintf(stderr, "sending election to %d failed\n", nextPort);
    
    seq ++;
    return 0;
}


int handle_election(struct msg* recv_msg){
    lastMSG = chrono::system_clock::now();
    struct msg_election* election = (struct msg_election*)recv_msg;
    if(nextPort == -1)
        return 0;
    if(election->bestCand == myPort){
        struct msg_elected electedMSG;
        electedMSG.msg_type = MSG_ELECTED;
        electedMSG.clientID = election->clientID;
        electedMSG.electionID = election->electionID;
        electedMSG.electedID = election->bestCand;
        leader = election->bestCand;
        output << lastMSG - start <<
            ": leader selected" << endl;
        return send_elected(&electedMSG);
    }

    if(myPort > election->bestCand){
        output << lastMSG - start <<
            ": relayed election message, replaced leader" << endl;
        election->bestCand = myPort;
    }
    forward_election(election);
    return 0;
}

int forward_election(struct msg_election* recv_msg){
    output << chrono::system_clock::now() - start <<
        ": relayed election message, leader: client " 
        << recv_msg->clientID << endl;

    ssize_t bytes;
    void *data;
    size_t len;

    data = recv_msg;
    len = sizeof *recv_msg;

    next_addr.sin_port = htons(nextPort);
    bytes = sendto(s, data, len, 0, (struct sockaddr*)&next_addr, sizeof next_addr);
    if(bytes < 0)
        fprintf(stderr, "sending election to %d failed\n", nextPort);
    
    seq ++;
    return 0;
}

int send_elected(struct msg_elected* electedMSG){
    ssize_t bytes;
    void *data;
    size_t len;

    data = electedMSG;
    len = sizeof *electedMSG;

    next_addr.sin_port = htons(nextPort);
    bytes = sendto(s, data, len, 0, (struct sockaddr*)&next_addr, sizeof next_addr);
    if(bytes < 0)
        fprintf(stderr, "sending election to %d failed\n", nextPort);
    
    seq ++;
    return 0;
}

int send_token(){
    lastToken = chrono::system_clock::now();
    ssize_t bytes;
    void *data;
    size_t len;

    data = &token;
    len = sizeof token;

    next_addr.sin_port = htons(nextPort);
    bytes = sendto(s, data, len, 0, (struct sockaddr*)&next_addr, sizeof next_addr);
    if(bytes < 0)
        fprintf(stderr, "sending election to %d failed\n", nextPort);
    // output << chrono::system_clock::now() - start <<
    //     ": token " << token.tokenID << " was sent to client " <<
    //     nextPort << endl;
    seq ++;
    return 0;
}

int handle_elected(struct msg* recv_msg){
    lastMSG = chrono::system_clock::now();
    struct msg_elected* electedMSG = (struct msg_elected*)recv_msg;
    if(tokenExist == 1)
        return 0;
    if(electedMSG->electedID == myPort && tokenExist == 0){
        token.msg_type = MSG_TOKEN;
        token.clientID = myPort;
        token.tokenID = electedMSG->electionID;
        output << lastMSG - start <<
            ": new token generated " << electedMSG->electionID << endl;
        tokenExist = 1;
        sending = 1;
        int more = send_text();
        if(more == -1)
            send_token();
        return 0;
    }
    return send_elected(electedMSG);
}

int handle_token(struct msg* recv_msg){
    lastMSG = chrono::system_clock::now();
    struct msg_token* tokenp = (struct msg_token*)recv_msg;
    token.msg_type = tokenp->msg_type;
    token.clientID = tokenp->clientID;
    token.tokenID = tokenp->tokenID;
    // output << lastMSG - start <<
    //     ": token " << token.tokenID << " was received" << endl;
    sending = 1;
    tokenExist = 1;
    int more = send_text();
    if(more == -1)
        send_token();
    return 0;
}

int forward_text(struct msg_text* text){
    ssize_t bytes;
    void *data;
    size_t len;

    data = text;
    len = sizeof *text;

    next_addr.sin_port = htons(nextPort);
    bytes = sendto(s, data, len, 0, (struct sockaddr*)&next_addr, sizeof next_addr);
    if(bytes < 0)
        fprintf(stderr, "sending text to %d failed\n", nextPort);
    
    seq ++;
    return 0;
}

int send_text(){
    string line;
    if(input.is_open()){
        struct msg_text text;
        text.msg_type = MSG_text;
        text.sourceID = myPort;
        text.seqNum = seq;

        streampos pos = input.tellg();
        istream& result = getline(input, line);
        // nothing to send
        if(!result){
            sending = 0;
            return -1;
        }

        string timeStamp = line.substr(0, line.find("\t"));
        string contents = line.substr(line.find("\t")+1, line.find("\n"));
        string m = timeStamp.substr(0, timeStamp.find(":"));
        string s = timeStamp.substr(timeStamp.find(":")+1, timeStamp.find("\n"));
        chrono::seconds textTime(stoi(m) * 60 + stoi(s));
        if(textTime + start < chrono::system_clock::now()){
            strcpy(text.text, &contents[0]);
            forward_text(&text);
            sentTime = chrono::system_clock::now();
            output << sentTime - start <<
                ": text \"" << contents << "\" was sent" << endl;
        }else{
            input.seekg(pos);
            sending = 0;
            return -1;
        }
    }
    return 0;
}

int handle_text(struct msg* recv_msg, struct sockaddr_in sock){
    int source = htons(sock.sin_port);
    if(source != prevPort)
        return 0;
    lastMSG = chrono::system_clock::now();
    struct msg_text* text = (struct msg_text*)recv_msg;
    // a text that I sent
    if(text->sourceID == myPort){
        timeout = (chrono::system_clock::now() - sentTime) * 10;
        output << lastMSG - start << ": text \"" << text->text << 
            "\" was delievered to all successfully" << endl;
        int more = send_text();
        // no more msg to send
        if(more == -1){
            return send_token();
        }
        return more;
    }
    forward_text(text);
    output << lastMSG - start << ": text \"" << text->text <<
        "\" from client " << text->sourceID << " was relayed" << endl;

    return 0;
}

int send_recover(int sourceID){
    ssize_t bytes;
    void *data;
    size_t len;

    struct msg_recover recover;
    recover.msg_type = MSG_RECOVER;
    recover.sourceID = sourceID;

    data = &recover;
    len = sizeof recover;

    next_addr.sin_port = htons(nextPort);
    bytes = sendto(s, data, len, 0, (struct sockaddr*)&next_addr, sizeof next_addr);
    if(bytes < 0)
        fprintf(stderr, "sending election to %d failed\n", nextPort);
    
    seq ++;
    return 0;
}

int handle_recover(struct msg* recv_msg){
    struct msg_recover* recover = (struct msg_recover*)recv_msg;
    if(recover->sourceID == myPort)
        return 1;
    send_recover(recover->sourceID);
    send_probe();
    return 0;
}








