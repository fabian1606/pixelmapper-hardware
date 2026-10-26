//! Generated per-PID metadata (`pids::PID_META`, `pids::ENUMS`) agrees with protocol/pids.toml.

use pixelmapper_net::pids::{self, FieldType};

#[test]
fn every_pid_has_meta_and_vice_versa() {
    assert_eq!(pids::PID_META.len(), pids::PIDS.len());
    for def in pids::PIDS {
        let m =
            pids::pid_meta(def.pid).unwrap_or_else(|| panic!("no PID_META for 0x{:04X}", def.pid));
        assert_eq!(m.name, def.name);
        assert_eq!(m.device, def.device);
        assert_eq!(m.command_class, def.command_class);
        assert_eq!(m.get, def.command_class & 0x01 != 0);
        assert_eq!(m.set, def.command_class & 0x02 != 0);
        assert_eq!(m.listed, def.listed);
        let pdl = |f: Option<&[pids::PidField]>| {
            f.map(|f| f.iter().map(|x| x.max_size()).sum::<u16>() as u8)
        };
        assert_eq!(pdl(m.get_request), def.get_request_pdl, "{}", m.name);
        assert_eq!(pdl(m.get_response), def.get_response_pdl, "{}", m.name);
        assert_eq!(pdl(m.set_request), def.set_request_pdl, "{}", m.name);
        assert_eq!(pdl(m.set_response), def.set_response_pdl, "{}", m.name);
        match def.desc {
            Some(d) => {
                assert_eq!(m.pdl_size, Some(d.pdl_size));
                assert_eq!(m.data_type, Some(d.data_type));
                assert_eq!(m.unit, Some(d.unit));
                assert_eq!(m.prefix, Some(d.prefix));
                assert_eq!(m.min, Some(d.min as i64));
                assert_eq!(m.max, Some(d.max as i64));
                assert_eq!(m.default, Some(d.default as i64));
                assert_eq!(m.description, Some(d.description));
            }
            None => {
                assert_eq!(m.data_type, None);
                assert_eq!(m.description, None);
            }
        }
    }
    // spot check: every PID constant resolves
    for pid in [
        pids::PID_ENTER_BOOTLOADER,
        pids::PID_BOOT_INFO,
        pids::PID_NODE_HW_REVISION,
        pids::PID_PANEL_LOCK,
        pids::PID_FW_FINISH,
    ] {
        assert!(pids::pid_meta(pid).is_some());
    }
}

#[test]
fn bool_pids() {
    for pid in [
        pids::PID_PANEL_LOCK,
        pids::PID_PORT1_ENABLED,
        pids::PID_PORT4_ENABLED,
        pids::PID_PORT3_RDM_BG_DISCOVERY,
    ] {
        let m = pids::pid_meta(pid).unwrap();
        assert!(m.is_bool(), "{}", m.name);
        assert_eq!(m.value_field().unwrap().ty, FieldType::Bool);
        assert_eq!(m.set_request.unwrap()[0].ty, FieldType::Bool);
    }
    for pid in [
        pids::PID_PORT1_DMX_BREAK,
        pids::PID_NODE_HW_REVISION,
        pids::PID_FW_INFO,
    ] {
        assert!(!pids::pid_meta(pid).unwrap().is_bool());
    }
}

#[test]
fn ranged_single_value_pid() {
    let m = pids::pid_meta(pids::PID_PORT2_DMX_BREAK).unwrap();
    assert_eq!(m.name, "PORT2_DMX_BREAK");
    assert_eq!(m.base_name, "PORT_DMX_BREAK");
    assert_eq!(m.port, Some(2));
    assert!(m.get && m.set && m.listed);
    let f = m.value_field().unwrap();
    assert_eq!((f.name, f.ty), ("break_us", FieldType::U16));
    assert_eq!((m.min, m.max, m.default), (Some(176), Some(352), Some(176)));
    assert_eq!(m.data_type, Some(0x05)); // unsigned_word
    assert_eq!(m.unit, Some(0x15)); // second
    assert_eq!(m.prefix, Some(0x04)); // micro
    assert_eq!(m.description, Some("Port 2 DMX break"));
    assert_eq!(m.set_response, Some(&[][..]));
    assert_eq!(m.get_request, Some(&[][..]));

    let m = pids::pid_meta(pids::PID_DISPLAY_DIM_TIMEOUT).unwrap();
    assert_eq!((m.min, m.max, m.default), (Some(0), Some(3600), Some(60)));
    assert_eq!(m.port, None);
}

#[test]
fn repeat_expansion_all_ports() {
    for base in [
        "PORT_ENABLED",
        "PORT_DMX_BREAK",
        "PORT_DMX_MAB",
        "PORT_DMX_RATE",
        "PORT_DMX_MBS",
        "PORT_RDM_BG_DISCOVERY",
    ] {
        let ps: Vec<_> = pids::PID_META
            .iter()
            .filter(|m| m.base_name == base)
            .collect();
        assert_eq!(ps.len(), pids::NODE_PORTS as usize, "{base}");
        for (i, m) in ps.iter().enumerate() {
            assert_eq!(m.port, Some(i as u8 + 1));
            assert_eq!(m.pid, ps[0].pid + i as u16);
            let n = format!("{}", i + 1);
            assert!(
                m.name.contains(&n) && m.description.unwrap().contains(&n),
                "{}",
                m.name
            );
        }
    }
    assert_eq!(
        pids::pid_meta(pids::PID_PORT4_DMX_MBS).unwrap().name,
        "PORT4_DMX_MBS"
    );
}

#[test]
fn field_details_and_enums() {
    let m = pids::pid_meta(pids::PID_FW_WRITE).unwrap();
    assert!(!m.get && m.set && m.get_request.is_none() && m.get_response.is_none());
    let f = m.set_request.unwrap();
    assert_eq!((f[0].name, f[0].ty), ("offset", FieldType::U32));
    assert_eq!(
        (f[1].ty, f[1].len, f[1].max_len),
        (FieldType::Bytes, None, Some(224))
    );
    assert_eq!(f[1].max_size(), 224);
    assert_eq!(f[1].min_size(), 0);

    let m = pids::pid_meta(pids::PID_ENTER_BOOTLOADER).unwrap();
    let f = &m.set_request.unwrap()[0];
    assert_eq!(
        (f.ty, f.len, f.value),
        (FieldType::Ascii, Some(4), Some("BOOT"))
    );

    let m = pids::pid_meta(pids::PID_BOOT_CATCH_START).unwrap();
    let f = m.set_request.unwrap();
    assert_eq!((f[0].min, f[0].max), (Some(1), Some(4)));
    assert_eq!(f[1].ty, FieldType::Uid);
    assert!(m.value_field().is_none());

    let m = pids::pid_meta(pids::PID_FW_INFO).unwrap();
    let st = m
        .get_response
        .unwrap()
        .iter()
        .find(|f| f.name == "state")
        .unwrap();
    assert_eq!(st.enum_name, Some("fw_state"));
    let e = pids::enum_def("fw_state").unwrap();
    assert_eq!(e.value_name(pids::fw_state::READY), Some("READY"));
    assert_eq!(e.value_of("FAILED"), Some(pids::fw_state::FAILED));
    let m = pids::pid_meta(pids::PID_DISPLAY_ADDRESS_FORMAT).unwrap();
    assert_eq!(m.value_field().unwrap().enum_name, Some("address_format"));
    // a field enum reference that is not an [enums.*] table (BOOT_ERR_* tube constants) is kept raw
    let m = pids::pid_meta(pids::PID_BOOT_INFO).unwrap();
    let le = m
        .get_response
        .unwrap()
        .iter()
        .find(|f| f.name == "last_error")
        .unwrap();
    assert_eq!((le.enum_name, le.enum_ref), (None, Some("BOOT_ERR_*")));

    // every field enum_name resolves; every enum table is exported
    for m in pids::PID_META {
        for f in [m.get_request, m.get_response, m.set_request, m.set_response]
            .into_iter()
            .flatten()
            .flatten()
        {
            if let Some(e) = f.enum_name {
                assert!(pids::enum_def(e).is_some(), "{}.{}", m.name, f.name);
            }
        }
    }
    let names: Vec<_> = pids::ENUMS.iter().map(|e| e.name).collect();
    assert_eq!(
        names,
        ["boot_catch_state", "fw_state", "fw_error", "address_format"]
    );
    assert_eq!(pids::enum_def("fw_error").unwrap().values.len(), 13);
}

#[test]
fn serializes_camel_case() {
    let m = pids::pid_meta(pids::PID_PORT1_ENABLED).unwrap();
    let v = serde_json::to_value(m).unwrap();
    assert_eq!(v["baseName"], "PORT_ENABLED");
    assert_eq!(v["dataType"], 3);
    assert_eq!(v["setRequest"][0]["type"], "bool");
    let v = serde_json::to_value(pids::enum_def("address_format").unwrap()).unwrap();
    assert_eq!(v["values"][1]["name"], "PORT_ADDRESS");
}
