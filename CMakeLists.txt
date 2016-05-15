cmake_minimum_required(VERSION 3.5)
project(stcp_skeleton)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")

set(SOURCE_FILES
    client.c
    connection_demux.c
    connection_demux.h
    mysock.c
    mysock.h
    mysock_api.c
    mysock_hash.h
    mysock_impl.h
    network.c
    network.h
    network_io.c
    network_io.h
    network_io_socket.c
    network_io_socket.h
    network_io_tcp.c
    server.c
    stcp_api.c
    stcp_api.h
    tcp_sum.c
    tcp_sum.h
    transport.c
    transport.h)

add_executable(stcp_skeleton ${SOURCE_FILES})