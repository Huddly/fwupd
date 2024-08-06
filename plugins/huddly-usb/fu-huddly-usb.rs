// Copyright 2024 Lars erling stensen <Lars.erling.stensen@huddly.com>
// SPDX-License-Identifier: LGPL-2.1-or-later

#[derive(New, ValidateStream, ParseStream)]
struct FuStructHuddlyUsb {
    signature: u8 == 0xDE,
    address: u16le,
}

#[derive(ToString)]
enum FuHuddlyUsbStatus {
    Unknown,
    Failed,
}
