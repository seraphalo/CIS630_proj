#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <fcntl.h>
#include <string>
#include <map>
#include <iostream>
#include <time.h>


using namespace std;



//#include "hash.h"
#include "mychat.h"


#define MAX_CONNECTIONS 10
#define HOSTNAME_MAX 100
#define MAX_MESSAGE_LEN 65536
#define NUM_RECENT_ID 50

//typedef map<string,string> channel_type; //<username, ip+port in string>
typedef map<string,struct sockaddr_in> channel_type; //<username, sockaddr_in of user>

int s; //socket for listening
struct sockaddr_in server;


map<string,struct sockaddr_in> usernames; //<username, sockaddr_in of user>
map<string,int> active_usernames; //0-inactive , 1-active
//map<struct sockaddr_in,string> rev_usernames;
map<string,string> rev_usernames; //<ip+port in string, username>
map<string,channel_type> channels;

map<string, int> self_subs; // 0-not subed, 1-subed
int num_neighbors;
map<int, struct sockaddr_in> neighbors;
typedef map<int, int> channel_subs; // <index, 0-not subed, 1-subed>
map<string, channel_subs> neighbor_subs;
// map<string, int> subscribed; // 0-not subed, 1-subed

long recent_ids[NUM_RECENT_ID];
int num_ids = 0;

time_t start = time(NULL);
time_t last = start;



void handle_socket_input();
void handle_login_message(void *data, struct sockaddr_in sock);
void handle_logout_message(struct sockaddr_in sock);
void handle_join_message(void *data, struct sockaddr_in sock);
void handle_leave_message(void *data, struct sockaddr_in sock);
void handle_say_message(void *data, struct sockaddr_in sock);
void handle_list_message(struct sockaddr_in sock);
void handle_who_message(void *data, struct sockaddr_in sock);
void handle_keep_alive_message(struct sockaddr_in sock);
void send_error_message(struct sockaddr_in sock, string error_msg);
int send_s2s_join_message(request_s2s_join* req, struct sockaddr_in not_sending);
void handle_s2s_join_message(void *data, struct sockaddr_in sock);
int send_s2s_say_message(request_s2s_say* req, struct sockaddr_in source);
void handle_s2s_say_message(void *data, struct sockaddr_in sock);
int send_s2s_leave_message(const char* channel, struct sockaddr_in source);
void handle_s2s_leave_message(void *data, struct sockaddr_in sock);
int distribute_say_message(struct text_say* say);



int main(int argc, char *argv[]){
	
	if (argc < 3 || argc%2 == 0){
		printf("Usage: ./server domain_name port_num [domain_name port_num] ...\n");
		exit(1);
	}

	char hostname[HOSTNAME_MAX];
	int port;
	
	strcpy(hostname, argv[1]);
	port = atoi(argv[2]);


	int i;
	num_neighbors = (argc-3)/2;
	for(i=0; i<num_neighbors; i++){
		char neighbor_host[HOSTNAME_MAX];
		int neighbor_port;
		strcpy(neighbor_host, argv[i*2+3]);
		neighbor_port = atoi(argv[i*2+4]);

		struct sockaddr_in neighbor;
		neighbor.sin_family = AF_INET;
		neighbor.sin_port = htons(neighbor_port);

		struct hostent *he;
		if ((he = gethostbyname(neighbor_host)) == NULL) {
			puts("error resolving neighbor host name..");
			exit(1);
		}
		memcpy(&neighbor.sin_addr, he->h_addr_list[0], he->h_length);
		neighbors[i] = neighbor;
		std::cout << port << " neighbor host " << i << ": " << neighbor_host <<
			"port: " << neighbor_port << std::endl;
	}

	
	s = socket(PF_INET, SOCK_DGRAM, 0);
	if (s < 0){
		perror ("socket() failed\n");
		exit(1);
	}

	//struct sockaddr_in server;

	struct hostent     *he;

	server.sin_family = AF_INET;
	server.sin_port = htons(port);

	if ((he = gethostbyname(hostname)) == NULL) {
		puts("error resolving hostname..");
		exit(1);
	}
	memcpy(&server.sin_addr, he->h_addr_list[0], he->h_length);

	int err;

	err = bind(s, (struct sockaddr*)&server, sizeof server);

	if (err < 0)
		perror("bind failed\n");


	// create default channel Common
	string default_channel = "Common";
	map<string,struct sockaddr_in> default_channel_users;
	channels[default_channel] = default_channel_users;

	// subscribed["Common"] = 1;

	self_subs[default_channel] = 1;
	map<int, int> default_channel_subs;
	for(i=0; i<num_neighbors; i++)
		default_channel_subs[i] = 1;
	neighbor_subs[default_channel] = default_channel_subs;



	while(1){

		// use a file descriptor with a timer to handle timeouts
		int rc;
		fd_set fds;

		FD_ZERO(&fds);
		FD_SET(s, &fds);

		time_t current = time(NULL);
		if (current - last > 60){
			cout << "it has been " << current - last 
				<< " seconds" << endl;
			last = current;
			map<string, int>::iterator ss_it;
			for(ss_it = self_subs.begin(); 
				ss_it != self_subs.end(); ss_it ++){
				struct request_s2s_join * renew = (struct request_s2s_join*)malloc(
					sizeof(request_s2s_join));
				renew->req_type = REQ_S2S_JOIN;
				strcpy(renew->req_channel, ss_it->first.c_str());
				send_s2s_join_message(renew, server);
			}
		}
		
		// monitor mutiple file descriptor
		rc = select(s+1, &fds, NULL, NULL, NULL);
		
		if (rc < 0){
			printf("error in select\n");
            getchar();
		}
		else{
			int socket_data = 0;

			if (FD_ISSET(s,&fds)){
				// reading from socket
				handle_socket_input();
				socket_data = 1;
			}
		}
	}

	return 0;

}

void handle_socket_input(){

	struct sockaddr_in recv_client;
	ssize_t bytes;
	void *data;
	size_t len;
	socklen_t fromlen;
	fromlen = sizeof(recv_client);
	char recv_text[MAX_MESSAGE_LEN];
	data = &recv_text;
	len = sizeof recv_text;

	bytes = recvfrom(s, data, len, 0, (struct sockaddr*)&recv_client, &fromlen);


	if (bytes < 0)
		perror ("recvfrom failed\n");
	else{
		//printf("received message\n");

		struct request* request_msg;
		request_msg = (struct request*)data;

		//printf("Message type:");
		request_t message_type = request_msg->req_type;

		//printf("%d\n", message_type);

		if (message_type == REQ_LOGIN)
			handle_login_message(data, recv_client); //some methods would need recv_client
		else if (message_type == REQ_LOGOUT)
			handle_logout_message(recv_client);
		else if (message_type == REQ_JOIN)
			handle_join_message(data, recv_client);
		else if (message_type == REQ_LEAVE)
			handle_leave_message(data, recv_client);
		else if (message_type == REQ_SAY)
			handle_say_message(data, recv_client);
		else if (message_type == REQ_LIST)
			handle_list_message(recv_client);
		else if (message_type == REQ_WHO)
			handle_who_message(data, recv_client);
		else if (message_type == REQ_S2S_JOIN)
			handle_s2s_join_message(data, recv_client);
		else if (message_type == REQ_S2S_SAY)
			handle_s2s_say_message(data, recv_client);
		else if (message_type == REQ_S2S_LEAVE)
			handle_s2s_leave_message(data, recv_client);
		else
			//send error message to client
			send_error_message(recv_client, "*Unknown command");
		// cout << "----------------------------------------" << endl;

	}


}

void handle_s2s_leave_message(void *data, struct sockaddr_in sock){
	struct request_s2s_leave * msg = (struct request_s2s_leave*)data;
	string channel = msg->req_channel;
	int i;
	for(i=0; i<neighbors.size(); i++){
		if (neighbors[i].sin_addr.s_addr == sock.sin_addr.s_addr && 
			neighbors[i].sin_port == sock.sin_port)
			neighbor_subs[channel][i] = 0;
	}
	cout << inet_ntoa(server.sin_addr) << ":" << htons(server.sin_port) << " "
		<< inet_ntoa(sock.sin_addr) << ":" << htons(sock.sin_port)
		<< " recv S2S Leave " << msg->req_channel << endl;
}

void handle_s2s_say_message(void *data, struct sockaddr_in sock){
	struct request_s2s_say * msg = (struct request_s2s_say*)data;
	string channel = msg->req_channel;
	long id = msg->req_text_id;

	cout << inet_ntoa(server.sin_addr) << ":" << htons(server.sin_port) << " "
			<< inet_ntoa(sock.sin_addr) << ":" << htons(sock.sin_port)
			<< " recv S2S Say " << msg->req_username << " " << channel
			<< " \"" << msg->req_text << "\"" << endl;
	int i;
	for(i=0; i<min(num_ids,NUM_RECENT_ID); i++){
		if(recent_ids[i] == id){
			cout << "duplicate say and sending a leave" << endl;
			send_s2s_leave_message(channel.c_str(), sock);
			return;
		}
	}
	recent_ids[num_ids%NUM_RECENT_ID] = id;
	num_ids ++;

	if(neighbors.size() < 2 && channels[channel].size() == 0){
		send_s2s_leave_message(channel.c_str(), sock);
		return;
	}

	send_s2s_say_message(msg, sock);
	struct text_say say;
	say.txt_type = TXT_SAY;
	strcpy(say.txt_channel, msg->req_channel);
	strcpy(say.txt_username, msg->req_username);
	strcpy(say.txt_text, msg->req_text);
	distribute_say_message(&say);
}

int distribute_say_message(struct text_say* say){
	map<string,struct sockaddr_in> existing_channel_users;
	string channel = say->txt_channel;
	map<string,struct sockaddr_in>::iterator channel_user_iter;
	existing_channel_users = channels[channel];
	size_t len = sizeof *say;
	for(channel_user_iter = existing_channel_users.begin(); 
			channel_user_iter != existing_channel_users.end(); 
			channel_user_iter++){
		struct sockaddr_in send_sock = channel_user_iter->second;
		ssize_t bytes = sendto(s, say, len, 0, (struct sockaddr*)&send_sock, 
						sizeof send_sock);

		if (bytes < 0){
			perror("Message failed\n");
			return -1;
		}
	}
	return 0;
}

int send_s2s_leave_message(const char* channel, struct sockaddr_in source){
	struct request_s2s_leave * leave = (request_s2s_leave*)
		malloc(sizeof(request_s2s_leave));
	leave->req_type = REQ_S2S_LEAVE;
	strcpy(leave->req_channel, channel);
	size_t len = sizeof *leave;
	ssize_t sending = sendto(s, leave, len, 0, (struct sockaddr*)&source, sizeof source);

	if(sending < 0){
		perror("Message failed\n");
		return -1;
	}
	cout << inet_ntoa(server.sin_addr) << ":" << htons(server.sin_port) << " "
	<< inet_ntoa(source.sin_addr) << ":" << htons(source.sin_port)
	<< " send S2S Leave " << leave->req_channel << endl;

	return 0;
}

void handle_s2s_join_message(void *data, struct sockaddr_in sock){
	struct request_s2s_join * msg = (struct request_s2s_join*)data;
	string channel = msg->req_channel;
	cout << inet_ntoa(server.sin_addr) << ":" << htons(server.sin_port) << " "
			<< inet_ntoa(sock.sin_addr) << ":" << htons(sock.sin_port)
			<< " recv S2S Join " << channel << endl;

	map<string, channel_subs>::iterator it = neighbor_subs.find(channel);
	if(it == neighbor_subs.end()){
		// cout << htons(server.sin_port) << " sees " << channel 
		// 	<< " for the first time" << endl;
		// self_subs[channel] = 1;
		map<int, int> new_subs;
		int i;
		// subscribe every neighbor
		for(i=0; i<neighbors.size(); i++)
			new_subs[i] = 1;
		neighbor_subs[channel] = new_subs;
	}



	// have source subscribe
	int i;
	for(i=0; i<neighbors.size(); i++){
	channel_subs subs = neighbor_subs[channel];
		struct sockaddr_in nei_sock = neighbors[i];
		if(nei_sock.sin_addr.s_addr == sock.sin_addr.s_addr && 
			nei_sock.sin_port == sock.sin_port){
				subs[i] = 1;
			}
	}
		

	if(self_subs[channel] == 0){
		// cout << htons(server.sin_port) << " was not subscribed to "
		// 	<< channel << endl;
		self_subs[channel] = 1;
		channel_subs subs = neighbor_subs[channel];
		int i;
		// subscribe every neighbor
		for(i=0; i<channels.size(); i++)
			subs[i] = 1;


		struct request_s2s_join * s2s_join = (struct request_s2s_join*)malloc(
			sizeof(request_s2s_join));
		s2s_join->req_type = REQ_S2S_JOIN;
		strcpy(s2s_join->req_channel, channel.c_str());
		send_s2s_join_message(s2s_join, sock);
	}
}

void handle_login_message(void *data, struct sockaddr_in sock){
	struct request_login* msg;
	msg = (struct request_login*)data;

	string username = msg->req_username;
	usernames[username]	= sock;
	active_usernames[username] = 1;

	// rev_usernames[sock] = username;

	// char *inet_ntoa(struct in_addr in);
	string ip = inet_ntoa(sock.sin_addr);
	// cout << "ip: " << ip <<endl;
	int port = sock.sin_port;
	// unsigned short short_port = sock.sin_port;
	// cout << "short port: " << short_port << endl;
	// cout << "port: " << port << endl;

 	char port_str[6];
 	sprintf(port_str, "%d", port);
	//cout << "port: " << port_str << endl;

	string key = ip + "." +port_str;
	// cout << "key: " << key <<endl;
	rev_usernames[key] = username;

	// cout << "server: " << username << " logs in" << endl;
	cout << inet_ntoa(server.sin_addr) << ":" << htons(server.sin_port) << " "
	<< ip << ":" << htons(port)
	<< " recv Request login " << username << " " << endl;
}

void handle_logout_message(struct sockaddr_in sock){

	// construct the key using sockaddr_in
	string ip = inet_ntoa(sock.sin_addr);
	// cout << "ip: " << ip <<endl;
	int port = sock.sin_port;

 	char port_str[6];
 	sprintf(port_str, "%d", port);
	// cout << "port: " << port_str << endl;

	string key = ip + "." +port_str;
	// cout << "key: " << key <<endl;

	// check whether key is in rev_usernames
	map <string,string> :: iterator iter;

	iter = rev_usernames.find(key);
	if(iter == rev_usernames.end())
		// send an error message saying not logged in
		send_error_message(sock, "Not logged in");
	else{
		// cout << "key " << key << " found."<<endl;
		string username = rev_usernames[key];
		rev_usernames.erase(iter);

		// remove from usernames
		map<string,struct sockaddr_in>::iterator user_iter;
		user_iter = usernames.find(username);
		usernames.erase(user_iter);

		// remove from all the channels if found
		map<string,channel_type>::iterator channel_iter;
		for(channel_iter = channels.begin(); channel_iter != channels.end(); 
			channel_iter++){
			// cout << "key: " << iter->first << " username: " << iter->second << endl;
			// channel_type current_channel = channel_iter->second;
			map<string,struct sockaddr_in>::iterator within_channel_iterator;
			within_channel_iterator = channel_iter->second.find(username);
			if (within_channel_iterator != channel_iter->second.end())
			{
				channel_iter->second.erase(within_channel_iterator);
			}

		}


		// remove entry from active usernames also
		// active_usernames[username] = 1;
		map<string,int>::iterator active_user_iter;
		active_user_iter = active_usernames.find(username);
		active_usernames.erase(active_user_iter);


		cout << "server: " << username << " logs out" << endl;
	}

}

void handle_join_message(void *data, struct sockaddr_in sock){
	// get message fields
	struct request_join* msg;
	msg = (struct request_join*)data;

	string channel = msg->req_channel;

	string ip = inet_ntoa(sock.sin_addr);

	int port = sock.sin_port;

 	char port_str[6];
 	sprintf(port_str, "%d", port);
	string key = ip + "." +port_str;


	// check whether key is in rev_usernames
	map <string,string> :: iterator iter;


	iter = rev_usernames.find(key);
	if(iter == rev_usernames.end())
		// ip+port not recognized - send an error message
		send_error_message(sock, "Not logged in");
	else{
		string username = rev_usernames[key];

		map<string,channel_type>::iterator channel_iter;

		channel_iter = channels.find(channel);

		active_usernames[username] = 1;

		if (channel_iter == channels.end()){
			// channel not found
			map<string,struct sockaddr_in> new_channel_users;
			new_channel_users[username] = sock;
			channels[channel] = new_channel_users;
			// cout << "creating new channel and joining" << endl;

		}
		else{
			// channel already exits
			channels[channel][username] = sock;
			// cout << "joining exisitng channel" << endl;


		}

		// cout << "server: " << username << " joins channel " << channel << endl;
		cout << inet_ntoa(server.sin_addr) << ":" << htons(server.sin_port) << " "
			<< ip << ":" << htons(port)
			<< " recv Request join " << username << " "
			<< channel << endl;

		self_subs[channel] = 1;
		map<string, channel_subs>::iterator n_subs_iter = neighbor_subs.find(channel);
		if (n_subs_iter == neighbor_subs.end()){
			// first time see the channel
			map<int, int> new_subs;
			int i;
			// subscribe every neighbor
			for(i=0; i<neighbors.size(); i++)
				new_subs[i] = 1;
			neighbor_subs[channel] = new_subs;
			// cout << "first time see channel" << endl;
		}

		struct request_s2s_join * s2s_join = (struct request_s2s_join*)malloc(
			sizeof(request_s2s_join));
		s2s_join->req_type = REQ_S2S_JOIN;
		strcpy(s2s_join->req_channel, channel.c_str());
		send_s2s_join_message(s2s_join, server);
		// cout << "after sending s2s join:\t" << neighbor_subs[channel].size() << endl;
	}


}

int send_s2s_join_message(request_s2s_join* req, struct sockaddr_in not_sending){
	channel_subs subs = neighbor_subs[req->req_channel];
	size_t len = sizeof *req;
	int i;
	for(i=0; i<neighbors.size(); i++){
		// let every neighbor subscribe the channel
		subs[i] = 1;
		struct sockaddr_in send_sock = neighbors[i];
		// cout << "sending s2s join to: " << htons(send_sock.sin_port) << endl;
		// cout << "its sub to " << req->req_channel << " is " 
		// 	<< subs[i] << endl;
		if(send_sock.sin_addr.s_addr == not_sending.sin_addr.s_addr && 
			send_sock.sin_port == not_sending.sin_port)
			continue;

		ssize_t sending = sendto(s, req, len, 0, (struct sockaddr*)&send_sock, 
						sizeof send_sock);

		if(sending < 0){
			perror("Message failed\n"); //error
			return -1;
		}

		cout << inet_ntoa(server.sin_addr) << ":" << htons(server.sin_port) << " "
			<< inet_ntoa(send_sock.sin_addr) << ":" << htons(send_sock.sin_port)
			<< " send S2S Join " << req->req_channel << endl;
	}
	return 0;

}


void handle_leave_message(void *data, struct sockaddr_in sock){

	// get message fields
	struct request_leave* msg;
	msg = (struct request_leave*)data;

	string channel = msg->req_channel;

	string ip = inet_ntoa(sock.sin_addr);

	int port = sock.sin_port;

 	char port_str[6];
 	sprintf(port_str, "%d", port);
	string key = ip + "." +port_str;


	// check whether key is in rev_usernames
	map <string,string> :: iterator iter;


	iter = rev_usernames.find(key);
	if (iter == rev_usernames.end() )
		// ip+port not recognized - send an error message
		send_error_message(sock, "Not logged in");
	else{
		string username = rev_usernames[key];

		map<string,channel_type>::iterator channel_iter;

		channel_iter = channels.find(channel);

		active_usernames[username] = 1;

		if(channel_iter == channels.end()){
			// channel not found
			send_error_message(sock, "No channel by the name " + channel);
			cout << "server: " << username << " trying to leave non-existent channel " 
				<< channel << endl;
		}
		else{
			//channel already exits
			//map<string,struct sockaddr_in> existing_channel_users;
			//existing_channel_users = channels[channel];
			map<string,struct sockaddr_in>::iterator channel_user_iter;
			channel_user_iter = channels[channel].find(username);

			if (channel_user_iter == channels[channel].end()){
				//user not in channel
				send_error_message(sock, "You are not in channel " + channel);
				cout << "server: " << username << " trying to leave channel " << channel  
					<< " where he/she is not a member" << endl;
			}
			else{
				channels[channel].erase(channel_user_iter);
				//existing_channel_users.erase(channel_user_iter);
				cout << "server: " << username << " leaves channel " << channel <<endl;

				//delete channel if no more users
				if (channels[channel].empty() && (channel != "Common"))
				{
					channels.erase(channel_iter);
					cout << "server: " << "removing empty channel " << channel <<endl;
				}
			}
		}
	}
}




void handle_say_message(void *data, struct sockaddr_in sock){

	// get message fields
	struct request_say* msg;
	msg = (struct request_say*)data;

	string channel = msg->req_channel;
	string text = msg->req_text;


	string ip = inet_ntoa(sock.sin_addr);

	int port = sock.sin_port;

 	char port_str[6];
 	sprintf(port_str, "%d", port);
	string key = ip + "." +port_str;


	// check whether key is in rev_usernames
	map <string,string> :: iterator iter;


	iter = rev_usernames.find(key);
	if(iter == rev_usernames.end()){
		// ip+port not recognized - send an error message
		send_error_message(sock, "Not logged in ");
	}
	else{
		string username = rev_usernames[key];

		cout << inet_ntoa(server.sin_addr) << ":" << htons(server.sin_port) << " "
			<< ip << ":" << htons(port)
			<< " recv Request say " << username << " "
			<< channel << " \"" << text << "\"" << endl;

		map<string,channel_type>::iterator channel_iter;

		channel_iter = channels.find(channel);

		active_usernames[username] = 1;

		if(channel_iter == channels.end()){
			// channel not found
			send_error_message(sock, "No channel by the name " + channel);
			cout << "server: " << username << 
				" trying to send a message to non-existent channel " << channel << endl;
		}
		else{
			// channel already exits
			// map<string,struct sockaddr_in> existing_channel_users;
			// existing_channel_users = channels[channel];
			map<string,struct sockaddr_in>::iterator channel_user_iter;
			channel_user_iter = channels[channel].find(username);

			if (channel_user_iter == channels[channel].end()){
				// user not in channel
				send_error_message(sock, "You are not in channel " + channel);
				cout << "server: " << username << " trying to send a message to channel " 
					<< channel  << " where he/she is not a member" << endl;
			}
			else{
				map<string,struct sockaddr_in> existing_channel_users;
				existing_channel_users = channels[channel];
				for(channel_user_iter = existing_channel_users.begin(); 
						channel_user_iter != existing_channel_users.end(); channel_user_iter++)
				{
					// cout << "key: " << iter->first << " username: " << iter->second << endl;

					ssize_t bytes;
					void *send_data;
					size_t len;

					struct text_say send_msg;
					send_msg.txt_type = TXT_SAY;

					const char* str = channel.c_str();
					strcpy(send_msg.txt_channel, str);
					str = username.c_str();
					strcpy(send_msg.txt_username, str);
					str = text.c_str();
					strcpy(send_msg.txt_text, str);
					// send_msg.txt_username, *username.c_str();
					// send_msg.txt_text,*text.c_str();
					send_data = &send_msg;

					len = sizeof send_msg;

					// cout << username <<endl;
					struct sockaddr_in send_sock = channel_user_iter->second;


					// bytes = sendto(s, send_data, len, 0, (struct sockaddr*)&send_sock, fromlen);
					bytes = sendto(s, send_data, len, 0, (struct sockaddr*)&send_sock, 
						sizeof send_sock);

					if (bytes < 0)
						perror("Message failed\n"); //error


				}
				// cout << "server: " << username << " sends say message in " << channel <<endl;

				struct request_s2s_say s2s_say;
				s2s_say.req_type = REQ_S2S_SAY;
				// strcpy(s2s_say->)
				srand(time(NULL));
				long id = rand();
				s2s_say.req_text_id = id;
				strcpy(s2s_say.req_username, username.c_str());
				strcpy(s2s_say.req_channel, channel.c_str());
				strcpy(s2s_say.req_text, text.c_str());
				send_s2s_say_message(&s2s_say, sock);
			}

		}

	}

}

int send_s2s_say_message(request_s2s_say* req, struct sockaddr_in source){
	size_t len = sizeof *req;
	channel_subs subs = neighbor_subs[req->req_channel];
	// cout << "when saying in channel: "<< req->req_channel  << endl;
	int i;
	for(i=0; i<neighbors.size(); i++){
		struct sockaddr_in send_sock = neighbors[i];
		// cout << "source is " << inet_ntoa(source.sin_addr) << "-"
		// 	<< htons(source.sin_port) << endl;
		// cout << "trying to send from " << inet_ntoa(server.sin_addr)
		// 	<< "-" << htons(server.sin_port)
		// 	<< " to " << inet_ntoa(send_sock.sin_addr) << "-"
		// 	<< htons(send_sock.sin_port) << endl;
		// cout << "same ip: " << (send_sock.sin_addr.s_addr == source.sin_addr.s_addr )<< endl;
		// cout << "same port: " << (send_sock.sin_port == source.sin_port) << endl;
		if (send_sock.sin_addr.s_addr == source.sin_addr.s_addr && 
			send_sock.sin_port == source.sin_port){
			cout << "This is the source, don't send" << endl;
			continue;
		}
		
		// cout << "its sub is " << subs[i] << endl;
		if(subs[i] == 0){
			cout << "not subscibred, don't send" << endl;
			continue;
		}
	
		ssize_t sending = sendto(s, req, len, 0, (struct sockaddr*)&send_sock, 
						sizeof send_sock);
		// cout << "something's being sent by " << htons(server.sin_port) << endl;

		if(sending < 0){
			perror("Message failed\n"); //error
			return -1;
		}

		cout << inet_ntoa(server.sin_addr) << ":" << htons(server.sin_port) << " "
			<< inet_ntoa(send_sock.sin_addr) << ":" << htons(send_sock.sin_port)
			<< " send S2S Say " << req->req_username << " " << req->req_channel 
			<< " \"" << req->req_text << "\"" << endl;
	}
	return 0;	
}


void handle_list_message(struct sockaddr_in sock){
	string ip = inet_ntoa(sock.sin_addr);

	int port = sock.sin_port;

 	char port_str[6];
 	sprintf(port_str, "%d", port);
	string key = ip + "." +port_str;


	// check whether key is in rev_usernames
	map <string,string> :: iterator iter;


	iter = rev_usernames.find(key);
	if (iter == rev_usernames.end() )
	{
		// ip+port not recognized - send an error message
		send_error_message(sock, "Not logged in ");
	}
	else
	{
		string username = rev_usernames[key];
		int size = channels.size();
		// cout << "size: " << size << endl;

		active_usernames[username] = 1;

		ssize_t bytes;
		void *send_data;
		size_t len;


		// struct text_list temp;
		struct text_list *send_msg = (struct text_list*)malloc(sizeof (struct text_list) 
				+ (size * sizeof(struct channel_info)));


		send_msg->txt_type = TXT_LIST;

		send_msg->txt_nchannels = size;


		map<string,channel_type>::iterator channel_iter;



		// struct channel_info current_channels[size];
		// send_msg.txt_channels = new struct channel_info[size];
		int pos = 0;

		for(channel_iter = channels.begin(); channel_iter != channels.end(); channel_iter++)
		{
			string current_channel = channel_iter->first;
			const char* str = current_channel.c_str();
			// strcpy(current_channels[pos].ch_channel, str);
			// cout << "channel " << str <<endl;
			strcpy(((send_msg->txt_channels)+pos)->ch_channel, str);
			// strcpy(((send_msg->txt_channels)+pos)->ch_channel, "hello");
			// cout << ((send_msg->txt_channels)+pos)->ch_channel << endl;

			pos++;

		}
		send_data = send_msg;
		len = sizeof (struct text_list) + (size * sizeof(struct channel_info));
		struct sockaddr_in send_sock = sock;


		// bytes = sendto(s, send_data, len, 0, (struct sockaddr*)&send_sock, fromlen);
		bytes = sendto(s, send_data, len, 0, (struct sockaddr*)&send_sock, sizeof send_sock);

		if (bytes < 0)
			perror("Message failed\n"); //error

		cout << "server: " << username << " lists channels"<<endl;
	}
}


void handle_who_message(void *data, struct sockaddr_in sock){

	// get message fields
	struct request_who* msg;
	msg = (struct request_who*)data;

	string channel = msg->req_channel;

	string ip = inet_ntoa(sock.sin_addr);

	int port = sock.sin_port;

 	char port_str[6];
 	sprintf(port_str, "%d", port);
	string key = ip + "." +port_str;


	// check whether key is in rev_usernames
	map <string,string> :: iterator iter;


	iter = rev_usernames.find(key);
	if (iter == rev_usernames.end() )
	{
		// ip+port not recognized - send an error message
		send_error_message(sock, "Not logged in ");
	}
	else
	{
		string username = rev_usernames[key];

		active_usernames[username] = 1;

		map<string,channel_type>::iterator channel_iter;

		channel_iter = channels.find(channel);

		if (channel_iter == channels.end())
		{
			// channel not found
			send_error_message(sock, "No channel by the name " + channel);
			cout << "server: " << username << 
				" trying to list users in non-existing channel " << channel << endl;

		}
		else
		{
			// channel exits
			map<string,struct sockaddr_in> existing_channel_users;
			existing_channel_users = channels[channel];
			int size = existing_channel_users.size();

			ssize_t bytes;
			void *send_data;
			size_t len;


			// struct text_list temp;
			struct text_who *send_msg = (struct text_who*)malloc(sizeof (struct text_who) 
					+ (size * sizeof(struct user_info)));

			send_msg->txt_type = TXT_WHO;

			send_msg->txt_nusernames = size;

			const char* str = channel.c_str();

			strcpy(send_msg->txt_channel, str);

			map<string,struct sockaddr_in>::iterator channel_user_iter;

			int pos = 0;

			for(channel_user_iter = existing_channel_users.begin(); 
					channel_user_iter != existing_channel_users.end(); 
					channel_user_iter++){
				string username = channel_user_iter->first;
				str = username.c_str();
				strcpy(((send_msg->txt_users)+pos)->us_username, str);
				pos++;
			}

			send_data = send_msg;
			len = sizeof(struct text_who) + (size * sizeof(struct user_info));
			struct sockaddr_in send_sock = sock;


			// bytes = sendto(s, send_data, len, 0, (struct sockaddr*)&send_sock, fromlen);
			bytes = sendto(s, send_data, len, 0, (struct sockaddr*)&send_sock, sizeof send_sock);

			if (bytes < 0)
				perror("Message failed\n");

			cout << "server: " << username << " lists users in channnel "<< channel << endl;
			}
	}
}



void send_error_message(struct sockaddr_in sock, string error_msg){
	ssize_t bytes;
	void *send_data;
	size_t len;

	struct text_error send_msg;
	send_msg.txt_type = TXT_ERROR;

	const char* str = error_msg.c_str();
	strcpy(send_msg.txt_error, str);

	send_data = &send_msg;

	len = sizeof send_msg;


	struct sockaddr_in send_sock = sock;



	bytes = sendto(s, send_data, len, 0, (struct sockaddr*)&send_sock, sizeof send_sock);

	if (bytes < 0)
		perror("Message failed\n"); //error
}






